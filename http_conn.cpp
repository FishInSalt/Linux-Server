//
// Created by lz on 1/6/21.
//

#include "http_conn.h"

const char *ok_200_title = "OK";

const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has "
                             "bad syntax or is inherently impossible to satisfy.\n";

const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file from this server.\n";

const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";

const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problems serving the requested file.\n";

/*网站的根目录*/
const char *doc_root = "/var/www/html";

/*设置非阻塞的文件描述符，用于ET模式或者oneshot*/
int setnonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

void addfd(int epoll_fd, int fd, bool one_shot) {
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    if (one_shot) {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

void removefd(int epoll_fd, int fd) {
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

void modfd(int epoll_fd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event);
}

//通过两个静态变量记录用户数量以及使用的epoll内核时事件表
int http_conn::m_user_count = 0;
int http_conn::m_epoll_fd = -1;

void http_conn::close_conn(bool read_close) {
    if (read_close && m_sock_fd != -1) {
        removefd(m_epoll_fd, m_sock_fd);
        m_sock_fd = -1;
        m_user_count--;
    }

}

void http_conn::init(int sockfd, const sockaddr_in &addr) {
    m_sock_fd = sockfd;
    m_address = addr;
    /*以下两行是为了避免TIME_WAIT状态，仅用于调试*/
    int reuse = 1;
    setsockopt(m_sock_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);

    addfd(m_epoll_fd, m_sock_fd, true);
    m_user_count++;

    init();
}

void http_conn::init() {
    /*当前状态是检查请求行*/
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;

    m_method = GET;
    m_url = nullptr;
    m_version = nullptr;
    m_content_length = 0;
    m_host = nullptr;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}


/*解析一行的内容*/
http_conn::LINE_STATUS http_conn::parse_line() {
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx) {
        temp = m_read_buf[m_checked_idx];

        /*最后一个检测的字节索引的值为'\r'*/
        if (temp == '\r') {
            /*这次分析没读完一个完整的行，返回LINE_OPEN表示还需要继续读取数据*/
            if ((m_checked_idx + 1) == m_read_idx) {
                return LINE_OPEN;
            } else if (m_read_buf[m_checked_idx + 1] == '\n') {
                /*如果下一个字符是'\n'，即换行符，则说明我们读取到一个完整的行*/
                //把'\r\n'设为'\0'
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
        }
            /*另一种读取完整行的可能，'\n'在下一次读取的数据中*/
        else if (temp == '\n') {
            if ((m_checked_idx > 1) && m_read_buf[m_checked_idx - 1] == '\r') {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }

    }

    return LINE_OPEN;

}

/*循环读取客户数据，直到无数据可读*/
bool http_conn::read() {
    if (m_read_idx >= READ_BUFFER_SIZE)
        return false;

    int bytes_read = 0;
    while (true) {
        bytes_read = recv(m_sock_fd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if (bytes_read == -1) {
            /*以下情况是读到无数据可读了*/
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            return false;
        }
            /*对方关闭信息， 看来返回0是对方关闭连接的专属字节数，如果是无数据可读的情况会返回-1，并将errno设为EAGAIN*/
        else if (bytes_read == 0) {
            return false;
        }
    }

    m_read_idx += bytes_read;
    return true;
}

/*解析请求行，获取请求方法、目标url、以及http版本号*/
http_conn::HTTP_CODE http_conn::parse_request_line(char *text) {
    /*strpbrk返回在参数1中找到的第一个出现在参数2中的任意字符的位置*/
    m_url = strpbrk(text, " \t");
    if (!m_url) {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';

    char *method = text;
    /*忽略大小写比较字符大小，相等返回0*/
    if (strcasecmp(method, "GET") == 0) {
        m_method = GET;
    } else {
        return BAD_REQUEST;
    }
    /*strspn(str1,str2)检索str1中第一个不在str2中出现的字符的位置*/
    m_url += strspn(m_url, " \t");

    m_version = strpbrk(m_url, " \t");
    if (!m_version) {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0) {
        return BAD_REQUEST;
    }
    if (strncasecmp(m_url, "http://", 7) == 0) { // strncasecmp(str1,str2,n)比较str1和str2的前n个字符是否相同
        m_url += 7;
        m_url = strchr(m_url, '/');
    }
    if (!m_url || m_url[0] != '/') {
        return BAD_REQUEST;
    }

    m_check_state = CHECK_STATE_HEADER;
    /*NO_REQUEST表示请求还不完整，需要继续处理客户数据*/
    return NO_REQUEST;

}

/*解析HTTP请求的一个头部信息*/
http_conn::HTTP_CODE http_conn::parse_headers(char *text) {
    /*如果遇到空行，表示头部解析完毕*/
    if (text[0] == '\0') {
        /*如果http请求有消息体,则还需要读取m_content_length字节的消息体，状态机转移到CHECK_STATE_CONTENT*/
        if (m_content_length != 0) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }

        /*否则说明得到了一个完整的http请求*/
        return GET_REQUEST;

    }
        /*处理connection头部字段*/
    else if (strncasecmp(text, "Connection:", 11) == 0) {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0) {
            m_linger = true;
        }
    }
        /*处理Content-Length头部字段*/
    else if (strncasecmp(text, "Content-Length:", 15) == 0) {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
        /*处理Host头部字段*/
    else if (strncasecmp(text, "Host:", 5) == 0) {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    } else {
        printf("oop! unknown header %s\n", text);
    }
    return NO_REQUEST;

}

/*此处没有解析http请求的消息体，只是判断是否被完整地读入*/
http_conn::HTTP_CODE http_conn::parse_contents(char *text) {
    if (m_read_idx >= (m_content_length + m_checked_idx)) {
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::process_read() {
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = nullptr;

    /*主状态机，用于从read_buffer中取出所有完整的行*/
    while (((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK))
           || ((line_status == parse_line()) == LINE_OK)) {
        text = get_line();
        m_start_line = m_checked_idx;
        printf("got 1 http line %s \n", text);
        switch (m_check_state) {
            case CHECK_STATE_REQUESTLINE: {
                ret = parse_request_line(text);
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER: {
                ret = parse_headers(text);
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                } else if (ret == GET_REQUEST) {
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT: {
                ret = parse_contents(text);
                if (ret == GET_REQUEST) {
                    return do_request();
                }
                line_status = LINE_OPEN;
                break;
            }

            default: {
                return INTERNAL_ERROR;
            }
        }
    }
    return NO_REQUEST;
}
/*当得到一个完整的、正确的HTTP请求时，我们就分析目标文件的属性，如果目标文件存在，对用户可读，且不是目录，
 * 则使用mmap将其映射到内存地址m_file_address处，并告诉调用者获取文件成功*/
http_conn::HTTP_CODE http_conn::do_request() {
    return http_conn::NO_REQUEST;
}
