// IOCP-HTTP.cpp: 定义控制台应用程序的入口点。
//

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <WinSock2.h>
#include <MSWSock.h>
#include <ws2tcpip.h>

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#include "http_parser.h"

static LPFN_CONNECTEX lpfnConnectEx = NULL;
static LPFN_ACCEPTEX  lpfnAcceptEx = NULL;
static struct socket_server *default_server = NULL;

//IO服务定义
struct socket_server {
    //完成端口数据
    HANDLE CompletionPort;

};
//HTTP协议头定义
struct http_header {
    char *k;
    char *v;
    struct http_header *next;
};
//HTTP请求定义
struct http_request {
    char *hostname;
    uint32_t ip;
    uint16_t port;

    uint16_t http_major;            //协议版本
    uint16_t http_minor;            //协议版本

    uint32_t method;                //请求类型
    
    struct http_header *header;     //请求头
    
    struct socket_server *server;   //执行请求的服务
};
//连接请求
struct request_connect {
    uintptr_t opaque;
    SOCKET fd;
};
//接收数据请求
struct request_recv {
    WSABUF buf;
    size_t RecvBytes;   //实际接收长度
    SOCKET fd;
};
//关闭连接请求
struct request_close {
    SOCKET fd;
};
//发送数据请求
struct request_send {
    SOCKET fd;
    WSABUF buf;
};
//DNS解析
struct request_dns {
    ADDRINFOEX Hints;
    PADDRINFOEX QueryResults;
    HANDLE CancelHandle;
};
//完成结构
typedef struct
{
    OVERLAPPED overlapped;      //系统对象
    uint32_t Type;              //请求类型
    union {
        char buffer[256];
        struct request_connect connect;
        struct request_recv recv;
        struct request_send send;
        struct request_close close;
        struct request_dns dns;
    } u;
}*LIO_DATA, IO_DATA;

//异步DNS回调
VOID WINAPI QueryCompleteCallback(DWORD Error, DWORD Bytes, IO_DATA *Overlapped) {
    printf("");
}
//IOCP线程
static int __stdcall IOCP_Thread(struct socket_server * ss) {
    for (; ;)
    {
        void *lpContext = NULL;
        IO_DATA        *pOverlapped = NULL;
        DWORD            dwBytesTransfered = 0;
        BOOL bReturn = GetQueuedCompletionStatus(ss->CompletionPort, &dwBytesTransfered, (LPDWORD)&lpContext, (LPOVERLAPPED *)&pOverlapped, INFINITE);
        if (!pOverlapped)
            return 1;
        if (bReturn == 0) {
            //请求失败
            switch (pOverlapped->Type)
            {
            case 'C': //连接服务器
            {
                //关闭套接字
                closesocket(pOverlapped->u.connect.fd);
                break;
            }
            case 'R'://收到数据
            {
                //关闭套接字
                closesocket(pOverlapped->u.recv.fd);
                //回收缓冲
                free(pOverlapped->u.recv.buf.buf);
                break;
            }
            case 'S'://发送数据
            {
                //
                closesocket(pOverlapped->u.send.fd);

                free(pOverlapped->u.send.buf.buf);
            }
            case 'D': //DNS解析
            {

            }
            default:
                return 0;
                break;
            }
            goto _ret;
        }

        switch (pOverlapped->Type)
        {
        case 'C': //连接服务器
        {
            //连接成功,投递接收请求
            //投递一个请求
            IO_DATA *msg = malloc(sizeof(*msg));
            memset(msg, 0, sizeof(*msg));
            msg->Type = 'R';
            msg->u.recv.fd = pOverlapped->u.connect.fd;
            msg->u.recv.buf.len = 8192;
            msg->u.recv.buf.buf = malloc(8192);

            //投递一个接收请求
            DWORD dwBufferCount = 1, dwRecvBytes = 0, Flags = 0;
            if (WSARecv(pOverlapped->u.connect.fd, &msg->u.recv.buf, 1, &msg->u.recv.RecvBytes, &Flags, (LPWSAOVERLAPPED)msg, NULL) == SOCKET_ERROR) {
                int err = WSAGetLastError();
                if (err != WSA_IO_PENDING)
                {
                    //套接字错误
                    free(msg->u.recv.buf.buf);
                    free(msg);
                }
            }
            break;
        }
        case 'R'://收到数据
        {


            if (dwBytesTransfered == 0) {
                //被主动断开?


                free(pOverlapped->u.recv.buf.buf);
                closesocket(pOverlapped->u.recv.fd);
                break;
            }


            //投递一个请求
            IO_DATA *msg = malloc(sizeof(*msg));
            memset(msg, 0, sizeof(*msg));
            msg->Type = 'R';
            msg->u.recv.fd = pOverlapped->u.recv.fd;
            msg->u.recv.buf.len = 8192;
            msg->u.recv.buf.buf = malloc(8192);

            //投递一个接收请求
            DWORD dwBufferCount = 1, dwRecvBytes = 0, Flags = 0;
            if (WSARecv(pOverlapped->u.recv.fd, &msg->u.recv.buf, 1, &msg->u.recv.RecvBytes, &Flags, (LPWSAOVERLAPPED)msg, NULL) == SOCKET_ERROR) {
                int err = WSAGetLastError();
                if (err != WSA_IO_PENDING)
                {
                    //套接字错误
                    free(msg->u.recv.buf.buf);
                    free(msg);
                    //通知套接字错误

                }
            }
            break;
        }
        case 'S'://发送数据
        {
            free(pOverlapped->u.send.buf.buf);
            break;
        }


        case 'k'://关闭连接
        {
            closesocket(pOverlapped->u.close.fd);
            break;
        }
        case 'D': //DNS解析
        {

        }
        default:
            break;
        }
    _ret:
        //释放完成数据
        free(pOverlapped);
    }
}
//创建服务
__declspec(dllexport) struct socket_server * __stdcall IOCP_New() {
    struct socket_server *server = (struct socket_server *)malloc(sizeof(struct socket_server));
    memset(server, 0, sizeof(struct socket_server));
    //创建完成端口
    server->CompletionPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    //启动iocp线程
    CreateThread(NULL, NULL, IOCP_Thread, server, NULL, NULL);

    return server;
}
//IOCP初始化
__declspec(dllexport) int __stdcall IOCP_Init() {
    static uint32_t is_init = 0;
    if (is_init)
        return 1;
    //初始化套接字
    WORD wVersionRequested;
    WSADATA wsaData;
    wVersionRequested = MAKEWORD(2, 2);
    WSAStartup(wVersionRequested, &wsaData);
    //获取函数地址
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);

    DWORD dwBytes = 0;
    GUID GuidConnectEx = WSAID_CONNECTEX;
    if (SOCKET_ERROR == WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &GuidConnectEx, sizeof(GuidConnectEx), &lpfnConnectEx, sizeof(lpfnConnectEx), &dwBytes, 0, 0))
    {
        return;
    }
    dwBytes = 0;
    GUID GuidAcceptEx = WSAID_ACCEPTEX;
    if (SOCKET_ERROR == WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &GuidAcceptEx, sizeof(GuidAcceptEx), &lpfnAcceptEx, sizeof(lpfnAcceptEx), &dwBytes, 0, 0))
    {
        return;
    }
    closesocket(s);
    is_init = 1;
    return 1;
}
//IOCP主机解析
__declspec(dllexport) void __stdcall TOCP_Dns(struct socket_server * ss, char *name) {
    //投递一个请求
    IO_DATA *msg = malloc(sizeof(*msg));
    memset(msg, 0, sizeof(*msg));
    msg->Type = 'D';
    msg->u.dns.Hints.ai_family = AF_INET;
    int len = MultiByteToWideChar(CP_OEMCP, 0, name, -1, NULL, 0);
    wchar_t *wstr = malloc(sizeof(wchar_t)*len);
    //return len == MultiByteToWideChar(CP_OEMCP, 0, str, -1, wstr, len);
    GetAddrInfoExW(name, NULL, NS_DNS, NULL, &msg->u.dns.Hints, &msg->u.dns.QueryResults, NULL, msg, QueryCompleteCallback, &msg->u.dns.CancelHandle);
    if (WSAGetLastError() != WSA_IO_PENDING)
    {
        //异常
    }
}
//IOCP反初始化
__declspec(dllexport) void __stdcall IOCP_UnInit() {

}



//HTTP初始化
__declspec(dllexport) int __stdcall HTTP_Init() {
    static uint32_t is_init = 0;
    if (is_init)
        return 1;

    IOCP_Init();
    default_server = IOCP_New();

    is_init = 1;
    return 1;
}
//创建HTTP请求
__declspec(dllexport) struct http_request * __stdcall HTTP_Request_New() {
    struct http_request *requset = (struct http_request *)malloc(sizeof(struct http_request));
    if (!requset)
        return NULL;
    requset->http_major = 1;
    requset->http_minor = 1;
    requset->method = HTTP_GET;
    requset->port = 80;
    requset->server = default_server;

    return requset;
}
//发送请求
__declspec(dllexport) void __stdcall HTTP_Request_Send(struct http_request *requset) {
    if (!requset)
        return;
    //合成请求数据


}
//销毁请求
__declspec(dllexport) struct http_request * __stdcall HTTP_Request_Delete(struct http_request *requset) {

    if (requset->hostname)
        free(requset->hostname);
    free(requset);
}
//HTTP反初始化
__declspec(dllexport) void __stdcall HTTP_UnInit() {

}


int main()
{
    HTTP_Init();
    
    TOCP_Dns(default_server, "www.baidu.com");

    scanf("%s");
    return 0;
}

