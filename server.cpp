#include<arpa/inet.h>
#include<fcntl.h>
#include<sys/epoll.h>
#include<sys/socket.h>
#include<sys/types.h>
#include<netdb.h>
#include<unistd.h>
#include<fmt/format.h>
#include<string.h>
#include<thread>
#include<vector>
#include<string>
#include<string_view>
#include<map>
#include <cassert>


int check_error(const char* msg,int res){
    if(res==-1){
        fmt::print("{}: {}\n",msg ,strerror(errno));
        throw;
    }
    return res;
}

size_t check_error(const char* msg,ssize_t res){
    if(res==-1){
        fmt::print("{}: {}\n",msg ,strerror(errno));
        throw;
    }
    return res;
}

//宏定义（Macro），简化函数调用时的错误检查，并自动捕获函数名和返回值进行统一处理
#define CHECK_CALL(func, ...) check_error(#func, func(__VA_ARGS__))

std::error_category const &gai_category() {
    static struct final : std::error_category {
        char const *name() const noexcept override {
            return "getaddrinfo";
        }

        std::string message(int err) const override {
            return gai_strerror(err);
        }
    } instance;
    return instance;
}

struct address_resolver {
    struct address_ref {
        struct sockaddr *m_addr;
        socklen_t m_addrlen;
    };

    struct address {
        //匿名union，用来同时兼容 IPv4、IPv6 或其他 socket 地址类型
        union {
            struct sockaddr m_addr;
            struct sockaddr_storage m_addr_storage;
        };
        socklen_t m_addrlen = sizeof(struct sockaddr_storage);

        operator address_ref() {
            return {&m_addr, m_addrlen};
        }
    };

    struct address_info {
        struct addrinfo *m_curr = nullptr;

        address_ref get_address() const {
            return {m_curr->ai_addr, m_curr->ai_addrlen};
        }

        int create_socket() const {
            int sockfd = CHECK_CALL(socket,m_curr->ai_family,m_curr->ai_socktype,m_curr->ai_protocol);
            return sockfd;
        }

        int create_socket_and_bind() const {
            int sockfd = create_socket();
            address_ref serve_addr = get_address();
            int on = 1;
            setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
            setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
            CHECK_CALL(bind, sockfd, serve_addr.m_addr, serve_addr.m_addrlen);
            CHECK_CALL(listen, sockfd, SOMAXCONN);
            return sockfd;
        }
        
        //[[nodiscard]] 编译器建议调用者不要忽略这个函数的返回值。如果你调用这个函数但没用它的返回值，可能会发出警告。
        [[nodiscard]] bool next_entry() {
            m_curr = m_curr->ai_next;
            if (m_curr == nullptr) {
                return false;
            }
            return true;
        }
    };

    struct addrinfo *m_head = nullptr;

    address_info resolve(std::string const &name, std::string const &service) {
        int err = getaddrinfo(name.c_str(), service.c_str(), NULL, &m_head);
        if (err != 0) {
            auto ec = std::error_code(err, gai_category());
            throw std::system_error(ec, name + ":" + service);
        }
        return {m_head};
    }

    address_resolver() = default;

    address_resolver(address_resolver &&that) : m_head(that.m_head) {
        that.m_head = nullptr;
    }

    ~address_resolver() {
        if (m_head) {
            freeaddrinfo(m_head);
        }
    }
};

std::vector<std::thread> pool;

using StringMap = std::map<std::string,std::string>;

//定义解析Http请求的类
//解决粘包问题
struct http11_header_parser{
    std::string m_header;       //整个header部分
    std::string m_heading_line;  //一行一行的headerline部分
    StringMap m_header_keys;    //存储首部行的kvpair
    std::string m_body;
    bool m_header_finished = false;

    [[nodiscard]] bool header_finished(){
        return m_header_finished;
    }

    //解析整个头部，不单纯是content_length一个首部行
    void _extract_headers(){
        std::string_view header = m_header;
        size_t pos = header.find("\r\n",0, 2);
        while (pos != std::string::npos) {
            // 跳过 "\r\n"
            pos += 2;
            // 从当前位置开始找，先找到下一行位置（可能为 npos）
            size_t next_pos = header.find("\r\n", pos,2);
            //如果line_len == std::string::npos，substr就代表“取到末尾”
            size_t line_len = std::string::npos;
            if (next_pos != std::string::npos) {
                // 如果下一行还不是结束，那么 line_len 设为本行开始到下一行之间的距离
                line_len = next_pos - pos;
            }
            // 就能切下本行
            std::string_view line = header.substr(pos, line_len);
            size_t colon = line.find(": ",0,2);
            if (colon != std::string::npos) {
                // 每一行都是 "键: 值"
                std::string key = std::string(line.substr(0, colon));
                std::string_view value = line.substr(colon + 2);
                //将key统一转为小写，因为http头部不区分大小写
                std::transform(key.begin(), key.end(), key.begin(), [] (char c) {
                    if ('A' <= c && c <= 'Z')
                        c += 'a' - 'A';
                    return c;
                });
                // 古代 C++ 过时的写法：m_header_keys[key] = value;
                // 现代 C++17 的高效写法：
                m_header_keys.insert_or_assign(std::move(key), value);
            }
            pos = next_pos;
        }
    } 

    void push_chunk(std::string_view chunk) {
        assert(!m_header_finished);
        size_t old_size = m_header.size();
        m_header.append(chunk);
        std::string_view header = m_header;
        // 如果还在解析头部的话，尝试判断头部是否结束
        // 从 old_size - 4 开始重新搜索 \r\n\r\n，防止跨块遗漏
        if (old_size < 4)
            old_size = 4;
        old_size -= 4;
        size_t header_len = header.find("\r\n\r\n", old_size, 4);
        if (header_len != std::string::npos) {
            // 头部已经结束
            m_header_finished = true;
            // 把不小心多读取的正文留下
            m_body = header.substr(header_len + 4);
            m_header.resize(header_len);
            // 开始分析头部，尝试提取 Content-length 字段
            _extract_headers();
        }
    }

    std::string &headline() {
        return m_heading_line;
    }

    StringMap &headers() {
        return m_header_keys;
    }

    std::string &headers_raw() {
        return m_header;
    }

    std::string &extra_body() {
        return m_body;
    }
};

template<class HeaderParser = http11_header_parser>
struct http_request_parser{
    HeaderParser m_header_parser;
    size_t m_content_length=0;
    bool m_body_finished = false;
    
    //判断整个请求是否结束
    [[nodiscard]] bool request_finished(){
        return m_body_finished;
    }

    //获取请求body（for post）
    std::string &body(){
        return m_header_parser.extra_body();
    }

    size_t _extract_content_length(){
        //取出请求中的headers的map
        auto & headers = m_header_parser.headers();
        auto it = headers.find("content-length");

        if(it == headers.end()){
            //没找到直接返回默认值0
            fmt::print("没找到content-length首部行\n");
            return 0;
        }
        try{
            fmt::print("找到了content-length首部行\n");
            return std::stoi(it->second);
        }catch(std::invalid_argument const&){
            return 0;
        }
    }

    void push_chunk(std::string_view chunk){
        if(!m_header_parser.header_finished()){
            //头部解析还没有完成
            m_header_parser.push_chunk(chunk);
            if(m_header_parser.header_finished()){
                fmt::print("头部已经解析完成，下一步获取正文长度\n");
                m_content_length = _extract_content_length();
                fmt::print("content-length的值为：{}\n",m_content_length);
                //防止body多读
                if(body().size()>=m_content_length){
                    m_body_finished = true;
                    body().resize(m_content_length);
                }
            }
        }else{
            body().append(chunk);
            if(body().size()>=m_content_length){
                m_body_finished = true;
                body().resize(m_content_length);
            }
        }
        
    }

};

int main(){
    //TCP基于stream的协议，容易出现粘包的问题
    setlocale(LC_ALL,"zh_CN.UTF-8");    //将strerror的信息转换为中文
    fmt::print("正在监听：127.0.0.1:8080\n");
    address_resolver resolver;
    auto entry = resolver.resolve("127.0.0.1","8080");
    //  在客户端开始listen后，操作系统开始监听连接请求，维护一个连接队列（backlog）
    //  三次握手，服务端会把客户端的详细地址和端口放入连接队列中
    //  调用accept时，操作系统会从内核队列中取出一个已经完成握手的连接，降低至存入clientaddr中并返回一个新的fd作为文件描述符，完成握手开始通信
    int listenfd = entry.create_socket_and_bind();  //监听文件描述符，与连接文件描述法不同，connid才可以进行读写
    
    while(true){
        address_resolver::address client_addr;
        int connid = CHECK_CALL(accept,listenfd,&client_addr.m_addr,&client_addr.m_addrlen);
        //必须按值(拷贝)进行捕获connid
        pool.emplace_back(([connid]{
            //operate the content readed
            char buf[1024];
            http_request_parser req_parser;
            do{
                size_t n = CHECK_CALL(read,connid,buf,sizeof(buf));
                req_parser.push_chunk(std::string_view(buf,n));

            }while(!req_parser.request_finished());

            fmt::print("收到请求头: {}\n",req_parser.m_header_parser.headers_raw());
            fmt::print("收到请求正文: {}\n",req_parser.body());

            std::string body = req_parser.body();

            // \r\n for newline in http header
            // double \r\n for the end of http header
            std::string res = "HTTP/1.1 200 OK\r\nServer: co_http\r\nConnection: close\r\nContent-length: " + std::to_string(body.size()) + "\r\n\r\n" + body;

            fmt::print("我的答复是: {}\n",res);
            CHECK_CALL(write,connid,res.data(),res.size());
            close(connid);

        }));
        
    }

    //调用join等待所有线程任务执行结束，主线程可能直接结束，导致子线程来不及完成工作，甚至程序异常终止
    for(auto&t:pool){
        t.join();
    }
    return 0;
    
    
} 