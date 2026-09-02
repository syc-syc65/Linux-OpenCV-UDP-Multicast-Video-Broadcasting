#include <opencv4/opencv2/opencv.hpp>
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <chrono>
#include <atomic>

using namespace cv;
using namespace std;

#define MULTICAST_PORT 9000
#define MAX_QUEUE_SIZE 1

#pragma pack(push,1)
struct UdpSliceHeader{
    int frame_id;
    short total_slice;
    short slice_id;
};
#pragma pack(pop)

const int SLICE_DATA_MAX = 1400;

template<typename T>
class ThreadSafeQueue
{
private:
    queue<T> m_q;
    mutex m_mtx;
    condition_variable m_cv;
public:
    void push(const T&val)
    {
        lock_guard<mutex> lock(m_mtx);
        if(m_q.size()>=MAX_QUEUE_SIZE)
            m_q.pop();
        m_q.push(val);
        m_cv.notify_one();
    }
    bool pop(T& out,int timeout_ms=90)
    {
        unique_lock<mutex> lock(m_mtx);
        if(!m_cv.wait_for(lock,chrono::milliseconds(timeout_ms),[this](){return !m_q.empty();}))
            return false;
        out=m_q.front();
        m_q.pop();
        return true;
    }
};

ThreadSafeQueue<Mat> g_frame_queue;
atomic<bool> g_running{true};

mutex g_disp_mtx;
Mat g_disp_frame;
bool g_disp_new = false;

void push_disp_frame(const Mat& frame)
{
    lock_guard<mutex> lock(g_disp_mtx);
    frame.copyTo(g_disp_frame);
    g_disp_new = true;
}

void frame_to_jpeg(const Mat &frame,vector<uchar>&out_buf,int quality=60)
{
    vector<int> param={IMWRITE_JPEG_QUALITY,quality};
    imencode(".jpg",frame,out_buf,param);
}

int create_multicast_socket()
{
    int sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if(sock_fd < 0) { perror("socket error"); return -1; }
    int sndbuf = 1024*1024;
    setsockopt(sock_fd,SOL_SOCKET,SO_SNDBUF,&sndbuf,sizeof(sndbuf));
    return sock_fd;
}

void udp_send_jpeg(int sock_fd,const sockaddr_in&dst_addr,const vector<uchar>&jpeg_data,int frame_id)
{
    int data_len=(int)jpeg_data.size();
    int total_silce=(data_len+SLICE_DATA_MAX-1)/SLICE_DATA_MAX;

    for(int slice_id=0;slice_id<total_silce;slice_id++)
    {
        UdpSliceHeader ush{};
        ush.frame_id=htonl(frame_id);
        ush.slice_id=htons((short)slice_id);
        ush.total_slice=htons((short)total_silce);

        int offset=slice_id*SLICE_DATA_MAX;
        int send_len = data_len - offset;
        if(send_len > SLICE_DATA_MAX)
            send_len = SLICE_DATA_MAX;

        char send_buf[sizeof(UdpSliceHeader)+SLICE_DATA_MAX];
        memset(send_buf,0,sizeof(send_buf));
        memcpy(send_buf,&ush,sizeof(UdpSliceHeader));
        memcpy(send_buf+sizeof(UdpSliceHeader),jpeg_data.data()+offset,send_len);
        int ret=sendto(sock_fd,send_buf,sizeof(UdpSliceHeader)+send_len,0,(struct sockaddr *)&dst_addr,sizeof(sockaddr_in));

        if(ret<0)
            perror("sendto failed");
        usleep(800);
    }
}

// 生产者：通过 ffmpeg x11grab 管道截屏（兼容 XWayland/Wayland）
void capture_thread()
{
    // ffmpeg 通过 x11grab 抓取屏幕，输出原始 bgr24 裸流到管道
    // 960x540 保证 JPEG 大小适中（桌面画面压缩率高，quality=30 足够）
    string pipeline = "ffmpeg -f x11grab -framerate 15 -video_size 960x540 -i :0.0 "
                      "-vf \"scale=960:540\" -f rawvideo -pix_fmt bgr24 - 2>/dev/null";
    VideoCapture cap(pipeline, CAP_FFMPEG);
    if(!cap.isOpened())
    {
        cout<<"[capture] ffmpeg x11grab 管道打开失败，尝试 x11grab 直接打开..."<<endl;
        // 备用：直接用 OpenCV 的 ffmpeg 后端打开 x11grab
        cap.open(":0.0", CAP_FFMPEG, {CAP_PROP_FRAME_WIDTH, 960,
                                      CAP_PROP_FRAME_HEIGHT, 540,
                                      CAP_PROP_FPS, 15});
    }
    if(!cap.isOpened())
    {
        cout<<"[capture] 截屏初始化失败！请检查 DISPLAY 环境变量和 ffmpeg 是否安装"<<endl;
        return;
    }
    cout<<"[capture] 屏幕采集已启动 (960x540 @15fps)"<<endl;

    Mat tmp;
    while(g_running)
    {
        if(!cap.read(tmp) || tmp.empty())
        {
            cout<<"[capture] 读取失败，屏幕采集可能被中断"<<endl;
            usleep(100000);
            continue;
        }
        push_disp_frame(tmp);
        g_frame_queue.push(tmp.clone());
    }
    cap.release();
}

void send_thread(int sock_fd,sockaddr_in dst_addr)
{
    Mat frame;
    int frame_id=0;
    while(g_running)
    {
        if(!g_frame_queue.pop(frame,50))
            continue;
        try
        {
            auto t1=chrono::high_resolution_clock::now();
            vector<uchar>jpeg_buf;
            frame_to_jpeg(frame,jpeg_buf,30);
            udp_send_jpeg(sock_fd,dst_addr,jpeg_buf,frame_id);
            auto t2=chrono::high_resolution_clock::now();
            auto cast=chrono::duration_cast<chrono::milliseconds>(t2-t1).count();
            cout<<"[send] frame:"<<frame_id<<" jpeg_size:"<<jpeg_buf.size()<<" cost:"<<cast<<"ms"<<endl;
            frame_id++;
        }
        catch(const exception& e)
        {
            cerr<<"[send] 帧处理异常(跳过): "<<e.what()<<endl;
        }
    }
}

int main(int argc, char const *argv[])
{
    int sock_fd=create_multicast_socket();
    if(sock_fd<0)
    {
        perror("socket error");
        return EXIT_FAILURE;
    }

    struct sockaddr_in dst_addr{};
    dst_addr.sin_family=AF_INET;
    dst_addr.sin_port=htons(MULTICAST_PORT);
    const char* dst_ip = (argc > 1) ? argv[1] : "10.210.198.146";
    if(inet_pton(AF_INET,dst_ip,&dst_addr.sin_addr)!=1)
    {
        printf("目标地址无效: %s\n", dst_ip);
        close(sock_fd);
        return EXIT_FAILURE;
    }
    bool is_multicast = (dst_ip[0]=='2' && dst_ip[1]=='2' && dst_ip[2]=='3' && dst_ip[3]=='9');
    if(is_multicast)
    {
        int ttl = 1;
        setsockopt(sock_fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    }
    printf("发送目标: %s:%d [%s]\n", dst_ip, MULTICAST_PORT, is_multicast?"组播模式":"单播模式");
    printf("屏幕采集模式，生产者消费者模型启动\n");

    thread producer(capture_thread);
    thread consumer(send_thread,sock_fd,dst_addr);

    bool preview_ok = true;
    namedWindow("采集端本地预览", WINDOW_NORMAL);
    while(g_running)
    {
        Mat disp;
        {
            lock_guard<mutex> lock(g_disp_mtx);
            if(g_disp_new) { g_disp_frame.copyTo(disp); g_disp_new=false; }
        }

        if(preview_ok)
        {
            try
            {
                if(!disp.empty()) imshow("采集端本地预览", disp);
                int key=waitKey(30);
                if(key==27) g_running=false;
                if(getWindowProperty("采集端本地预览", WND_PROP_VISIBLE) < 1)
                {
                    cout<<"[preview] 预览窗口被关闭，转入纯后台发送（停止请执行 pkill -x s）"<<endl;
                    preview_ok=false;
                }
            }
            catch(const cv::Exception& e)
            {
                cerr<<"[preview] GUI异常，转入纯后台发送: "<<e.what()<<endl;
                preview_ok=false;
            }
        }
        else
        {
            usleep(30000);
        }
    }

    producer.join();
    consumer.join();

    close(sock_fd);
    destroyAllWindows();
    return 0;
}
