//
// Created by lz on 1/6/21.
//

#ifndef WEBSERVERWITHTHREADPOOL_HTTP_CONN_H
#define WEBSERVERWITHTHREADPOOL_HTTP_CONN_H

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <sys/mman.h>
#include <stdarg.h>
#include <cerrno>
#include "lock.h"


class http_conn {
public:
    /*最大文件名长度*/
    static const int FILENAME_LEN = 200;
    /*读缓冲区的大小*/
    static const int READ_BUFFER_SIZE = 2048;
    /*写缓冲区大小*/
    static const int WRITE_BUFFER_SIZE = 1024;
    /*HTTP请求方法，目前该代码仅支持GET*/
    enum METHOD {
        GET = 0, POST, HEAD, PUT, DELETE, TRACE,
        OPTIONS, CONNECT, PATCH
    };
    /*解析客户请求时，主状态机所处的状态（这是一个有限状态机）*/
    enum CHECK_STATE {
        /*正在分析请求行*/
        CHECK_STATE_REQUESTLINE = 0,
        /*当前正在分析头部字段*/
        CHECK_STATE_HEADER,
        /*正在分析内容字段*/
        CHECK_STATE_CONTENT
    };
    /*HTTP请求的可能结果
     * 目前只用到
     * NO_REQUEST  不完整的请求
     * GET_REQUEST 完整请求
     * BAD_REQUEST 请求有语法错误*/
    enum HTTP_CODE {
        NO_REQUEST, GET_REQUEST, BAD_REQUEST,
        NO_RESOURCE, FORBIDDEN_REQUEST, FILE_REQUEST,
        INTERNAL_ERROR, CLOSED_CONNECTION
    };
    /*行的读取状态： 行完整，行出错， 行不完整*/
    enum LINE_STATUS {
        LINE_OK = 0, LINE_BAD, LINE_OPEN
    };

public:
    http_conn() = default;

    ~http_conn() = default;

    /*初始化新接收的连接*/
    void init(int sockfd, const sockaddr_in &addr);

    /*关闭连接*/
    void close_conn(bool read_close = true);

    /*处理客户连接*/
    void process();

    /*非阻塞读操作*/
    bool read();

    /*非阻塞写操作*/
    bool write();

private:
    /*初始化连接*/
    void init();

    /*解析HTTP请求*/
    HTTP_CODE process_read();

    /*填充HTTP应答*/
    bool process_write();


    /*下面一组函数被process_read调用以分析HTTP请求*/
    HTTP_CODE parse_request_line(char *text);

    HTTP_CODE parse_headers(char *text);

    HTTP_CODE parse_contents(char *text);

    HTTP_CODE do_request();

    char *get_line() { return m_read_buf + m_start_line; }

    LINE_STATUS parse_line();


    /*下面这组函数被processs_write调用以填充HTTP应答*/
    void unmap();

    bool add_response(const char *format, ...);

    bool add_content(const char *content);

    bool add_status_line(int status, const char *title);

    bool add_headers(int content_length);

    bool add_content_length(int content_length);

    bool add_linger();

    bool add_blank_line();

public:
    /*所有socket上的事件都在同一个epoll内核时间表中，所以epoll_fd被设为静态的*/
    static int m_epoll_fd;
    /*用户数量*/
    static int m_user_count;

private:
    /*该http conn的socket以及对方的地址*/
    int m_sock_fd;
    sockaddr_in m_address;

    /*读缓冲区*/
    char m_read_buf[READ_BUFFER_SIZE];
    /*标志读缓冲区中已读数据的最后一个字节的下一个字节*/
    int m_read_idx;
    /*当前分析的字符在缓冲区的位置*/
    int m_checked_idx;
    /*当前正在解析的行的开始位置*/
    int m_start_line;
    /*写缓冲区*/
    char m_write_buf[WRITE_BUFFER_SIZE];
    /*写缓冲区中待发送的数据*/
    int m_write_idx;

    /*主状态机所处的状态*/
    CHECK_STATE m_check_state;
    /*请求方法*/
    METHOD m_method;
    /*客户请求的目标文件的完整路径，其内容等于doc_root + m_url, doc_root 是网站根目录*/
    char m_real_file[FILENAME_LEN];
    /*客户请求的目标文件的文件名*/
    char *m_url;
    /*http协议的版本号，我们仅支持http/1.1*/
    char *m_version;
    /*主机名*/
    char *m_host;
    /*Http请求的消息体长度*/
    int m_content_length;
    /*http是否要求保持连接*/
    bool m_linger;
    /*客户请求的目标文件被mmap到内存的起始位置*/
    char *m_file_address;
    /*目标文件的状态，通过它我们可以判断文件是否存在，是否为目录等属性*/
    struct stat m_file_stat;
    /*采用writev来执行写操作，所以定义下面两个成员，其中m_iv_count表示被写内存块的数量*/
    struct iovec m_iv[2];

    int m_iv_count;


};


#endif //WEBSERVERWITHTHREADPOOL_HTTP_CONN_H
