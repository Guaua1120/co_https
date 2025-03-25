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
#include<cassert>
#include<deque>


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


[[noreturn]] void _throw_system_error(const char *what) {
    auto ec = std::error_code(errno, std::system_category());
    fmt::print(stderr, "{}: {} ({}.{})\n", what, ec.message(), ec.category().name(), ec.value());
    throw std::system_error(ec, what);
}

// errno 是一个在 C/C++ 中常见的全局变量，用于表示最近一次系统调用或标准库函数发生的错误编号。
// 默认模板参数 Except，表示某个允许“忽略”的错误号（errno 值）。
template <int Except =0,class T>
T check_error(const char* what ,T res){
    if(res==-1){
        if constexpr(Except !=0){
            if(errno == Except){
                //说明是我们可以接受的错误类型，就不报错，直接返回
                return -1;
            }
        }
        auto ec = std::error_code(errno,std::system_category());
        fmt::print(stderr,"{}: {}\n",what,ec.message());
        _throw_system_error(what);
    }
    return res;
}


//宏定义
//优化错误处理，输出具体的文件和行号信息
#define SOURCE_INFO_IMPL(file,line) "In " file ":" #line ": "
//传入两个预定义宏，表示源文件名和代码行号
#define SOURCE_INFO() SOURCE_INFO_IMPL(__FILE__,__LINE__)
//简化函数调用时的错误检查，并自动捕获函数名和返回值进行统一处理
// SOURCE_INFO() #func是字符串字面量的自动拼接
#define CHECK_CALL(func, ...) check_error(SOURCE_INFO() #func, func(__VA_ARGS__))
#define CHECK_CALL_EXCEPT(except,func, ...) check_error<except>(SOURCE_INFO() #func, func(__VA_ARGS__))


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

using StringMap = std::map<std::string,std::string>;


//只读字节流视图（char const *保证只读）
struct bytes_const_view {
    char const *m_data;
    size_t m_size;

    char const *data() const noexcept {
        return m_data;
    }

    size_t size() const noexcept {
        return m_size;
    }

    char const *begin() const noexcept {
        return data();
    }

    char const *end() const noexcept {
        return data() + size();
    }

    bytes_const_view subspan(size_t start, size_t len = static_cast<size_t>(-1)) const {
        if (start > size())
            throw std::out_of_range("bytes_const_view::subspan");
        if (len > size() - start)
            len = size() - start;
        return {data() + start, len};
    }

    operator std::string_view() const noexcept {
        return std::string_view{data(), size()};
    }
};


//可读可写字节流视图
struct bytes_view {
    char *m_data;
    size_t m_size;

    char *data() const noexcept {
        return m_data;
    }

    size_t size() const noexcept {
        return m_size;
    }

    char *begin() const noexcept {
        return data();
    }

    char *end() const noexcept {
        return data() + size();
    }

    bytes_view subspan(size_t start, size_t len) const {
        if (start > size())
            throw std::out_of_range("bytes_view::subspan");
        if (len > size() - start)
            len = size() - start;
        return {data() + start, len};
    }

    operator bytes_const_view() const noexcept {
        return bytes_const_view{data(), size()};
    }

    operator std::string_view() const noexcept {
        return std::string_view{data(), size()};
    }
};


//可变大小的字节缓冲区
struct bytes_buffer {
    std::vector<char> m_data;

    bytes_buffer() = default;
    bytes_buffer(bytes_buffer &&) = default;                //移动构造
    bytes_buffer &operator=(bytes_buffer &&) = default;     //移动赋值
    explicit bytes_buffer(bytes_buffer const &) = default;  //显式构造

    explicit bytes_buffer(size_t n) : m_data(n) {}

    //char const * 只读指针，内容不可变
    char const *data() const noexcept {
        return m_data.data();
    }

    char *data() noexcept {
        return m_data.data();
    }

    size_t size() const noexcept {
        return m_data.size();
    }

    
    //支持可读可写的迭代器
    char const *begin() const noexcept {
        return data();
    }

    char *begin() noexcept {
        return data();
    }

    char const *end() const noexcept {
        return data() + size();
    }

    char *end() noexcept {
        return data() + size();
    }

    //支持可读/可写的子视图
    //视图对象不持有数据，只是对数据的引用
    //先类型转换成bytes_const_view，再创建视图
    bytes_const_view subspan(size_t start, size_t len) const {
        return operator bytes_const_view().subspan(start, len);
    }

    bytes_view subspan(size_t start, size_t len) {
        return operator bytes_view().subspan(start, len);
    }

    //支持隐式类型转换
    operator bytes_const_view() const noexcept {
        return bytes_const_view{m_data.data(), m_data.size()};
    }

    operator bytes_view() noexcept {
        return bytes_view{m_data.data(), m_data.size()};
    }

    operator std::string_view() const noexcept {
        return std::string_view{m_data.data(), m_data.size()};
    }

    void append(bytes_const_view chunk) {
        m_data.insert(m_data.end(), chunk.begin(), chunk.end());
    }

    void append(std::string_view chunk) {
        m_data.insert(m_data.end(), chunk.begin(), chunk.end());
    }

    //追加字符串字面量，但 自动忽略末尾的 \0
    template <size_t N>
    void append_literial(const char (&literial)[N]) {
        append(std::string_view{literial, N - 1});
    }

    void clear() {
        m_data.clear();
    }

    void resize(size_t n) {
        m_data.resize(n);
    }

    void reserve(size_t n) {
        m_data.reserve(n);
    }
};



//定义解析Http数据包的类
//解决粘包问题
struct http11_header_parser{
    bytes_buffer m_header;       //整个header部分
    std::string m_header_line;  //第一行的请求的包含方法名和http版本的headerline部分
    StringMap m_header_keys;    //存储首部行的kvpair
    std::string m_body;
    bool m_header_finished = false;

    void reset_state() {
        m_header.clear();
        m_header_line.clear();
        m_header_keys.clear();
        m_body.clear();
        m_header_finished = 0;
    }

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

    void push_chunk(bytes_const_view chunk) {
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
        return m_header_line;
    }

    StringMap &headers() {
        return m_header_keys;
    }

    bytes_buffer &headers_raw() {
        return m_header;
    }

    std::string &extra_body() {
        return m_body;
    }
};

// 静态的固定长度的字节流的缓冲区
// N是模板参数
template <size_t N>
struct static_bytes_buffer {
    std::array<char, N> m_data;

    char const *data() const noexcept {
        return m_data.data();
    }

    char *data() noexcept {
        return m_data.data();
    }

    static constexpr size_t size() noexcept {
        return N;
    }

    operator bytes_const_view() const noexcept {
        return bytes_const_view{m_data.data(), N};
    }

    operator bytes_view() noexcept {
        return bytes_view{m_data.data(), N};
    }

    operator std::string_view() const noexcept {
        return std::string_view{m_data.data(), m_data.size()};
    }
};


//http请求和响应的数据包解析的基类，里面的HeaderParser头解析器共用http11_header_parser
template <class HeaderParser = http11_header_parser>
struct _http_base_parser {
    HeaderParser m_header_parser;
    size_t m_content_length = 0;
    size_t body_accumulated_size = 0;
    bool m_body_finished = false;

    void reset_state() {
        m_header_parser.reset_state();
        m_content_length = 0;
        body_accumulated_size = 0;
        m_body_finished = false;
    }

    [[nodiscard]] bool header_finished() {
        return m_header_parser.header_finished();
    }

    [[nodiscard]] bool request_finished() {
        return m_body_finished;
    }

    std::string &headers_raw() {
        return m_header_parser.headers_raw();
    }

    std::string &headline() {
        return m_header_parser.headline();
    }

    StringMap &headers() {
        return m_header_parser.headers();
    }


    //分别获取http请求/响应头headerline中的用空格分隔的三个部分
    //eg:一般的http请求为：headerline = "GET /index.html HTTP/1.1" (method + url + version )
    //响应为：headerline = "HTTP/1.1 200 OK"  (version + status + despcription)

    std::string _headline_first() {
        // "GET / HTTP/1.1" request
        // "HTTP/1.1 200 OK" response
        auto &line = headline();
        size_t space = line.find(' ');
        if (space == std::string::npos) {
            return "";
        }
        return line.substr(0, space);
    }

    std::string _headline_second() {
        // "GET / HTTP/1.1"
        auto &line = headline();
        size_t space1 = line.find(' ');
        if (space1 != std::string::npos) {
            return "";
        }
        size_t space2 = line.find(' ', space1);
        if (space2 != std::string::npos) {
            return "";
        }
        return line.substr(space1, space2);
    }

    std::string _headline_third() {
        // "GET / HTTP/1.1"
        auto &line = headline();
        size_t space1 = line.find(' ');
        if (space1 != std::string::npos) {
            return "";
        }
        size_t space2 = line.find(' ', space1);
        if (space2 != std::string::npos) {
            return "";
        }
        return line.substr(space2);
    }

    std::string &body() {
        return m_header_parser.extra_body();
    }

    size_t _extract_content_length() {
        auto &headers = m_header_parser.headers();
        auto it = headers.find("content-length");
        if (it == headers.end()) {
            return 0;
        }
        try {
            return std::stoi(it->second);
        } catch (std::logic_error const &) {
            return 0;
        }
    }

    void push_chunk(bytes_const_view chunk) {
        assert(!m_body_finished);
        if (!m_header_parser.header_finished()) {
            m_header_parser.push_chunk(chunk);
            if (m_header_parser.header_finished()) {
                body_accumulated_size = body().size();
                m_content_length = _extract_content_length();
                if (body_accumulated_size >= m_content_length) {
                    m_body_finished = true;
                }
            }
        } else {
            body().append(chunk);
            body_accumulated_size += chunk.size();
            if (body_accumulated_size >= m_content_length) {
                m_body_finished = true;
            }
        }
    }

    std::string read_some_body() {
        return std::move(body());
    }
};

//继承自基类base_parser的http_request_parser
template <class HeaderParser = http11_header_parser>
struct http_request_parser : _http_base_parser<HeaderParser> {
    std::string method() {
        return this->_headline_first();
    }

    std::string url() {
        return this->_headline_second();
    }

    std::string http_version() {
        return this->_headline_third();
    }
};

//继承自基类base_parser的http_response_parser
template <class HeaderParser = http11_header_parser>
struct http_response_parser : _http_base_parser<HeaderParser> {
    std::string http_version() {
        return this->_headline_first();
    }

    int status() {
        auto s = this->_headline_second();
        try {
            return std::stoi(s);
        } catch (std::logic_error const &) {
            return -1;
        }
    }

    std::string status_string() {
        return this->_headline_third();
    }
};

// 构造http的数据包头的writer
struct http11_header_writer {
    bytes_buffer m_buffer;

    void reset_state() {
        m_buffer.clear();
    }

    bytes_buffer &buffer() {
        return m_buffer;
    }

    void begin_header(std::string_view first, std::string_view second, std::string_view third) {
        m_buffer.append(first);
        m_buffer.append_literial(" ");
        m_buffer.append(second);
        m_buffer.append_literial(" ");
        m_buffer.append(third);
    }

    void write_header(std::string_view key, std::string_view value) {
        m_buffer.append_literial("\r\n");
        m_buffer.append(key);
        m_buffer.append_literial(": ");
        m_buffer.append(value);
    }

    void end_header() {
        m_buffer.append_literial("\r\n\r\n");
    }
};

//http请求writer和响应writer的基类
template <class HeaderWriter = http11_header_writer>
struct _http_base_writer {
    HeaderWriter m_header_writer;

    void _begin_header(std::string_view first, std::string_view second, std::string_view third) {
        m_header_writer.begin_header(first, second, third);
    }

    void reset_state() {
        m_header_writer.reset_state();
    }

    bytes_buffer &buffer() {
        return m_header_writer.buffer();
    }

    void write_header(std::string_view key, std::string_view value) {
        m_header_writer.write_header(key, value);
    }

    void end_header() {
        m_header_writer.end_header();
    }

    void write_body(std::string_view body) {
        m_header_writer.buffer().append(body);
    }
};

//这里实现response的parser和request的writer是为了实现中间代理服务器的功能
template <class HeaderWriter = http11_header_writer>
struct http_request_writer : _http_base_writer<HeaderWriter> {
    void begin_header(int status) {
        this->_begin_header("HTTP/1.1", std::to_string(status), "OK");
    }
};

template <class HeaderWriter = http11_header_writer>
struct http_response_writer : _http_base_writer<HeaderWriter> {
    void begin_header(int status) {
        this->_begin_header("HTTP/1.1", std::to_string(status), "OK");
    }
};


// Args... 是一个 参数包,可以接受任意数量的模板参数
template <class ...Args>
struct callback {
    struct _callback_base {
        virtual void _call(Args... args) = 0;
        virtual ~_callback_base() = default;
    };

    template <class F>
    struct _callback_impl final : _callback_base {
        F m_func;

        template <class ...Ts, class = std::enable_if_t<std::is_constructible_v<F, Ts...>>>
        _callback_impl(Ts &&...ts) : m_func(std::forward<Ts>(ts)...) {}

        void _call(Args... args) override {
            m_func(std::forward<Args>(args)...);
        }
    };

    std::unique_ptr<_callback_base> m_base;

    template <class F, class = std::enable_if_t<std::is_invocable_v<F, Args...> && !std::is_same_v<std::decay_t<F>, callback>>>
    callback(F &&f) : m_base(std::make_unique<_callback_impl<std::decay_t<F>>>(std::forward<F>(f))) {}

    callback() = default;

    callback(callback const &) = delete;
    callback &operator=(callback const &) = delete;
    callback(callback &&) = default;
    callback &operator=(callback &&) = default;

    void operator()(Args... args) const {
        assert(m_base);
        return m_base->_call(std::forward<Args>(args)...);
    }

    template <class F>
    F &target() const {
        assert(m_base);
        return static_cast<_callback_impl<F> &>(*m_base);
    }

    void *leak_address() {
        return static_cast<void *>(m_base.release());
    }

    static callback from_address(void *addr) {
        callback cb;
        cb.m_base = std::unique_ptr<_callback_base>(static_cast<_callback_base *>(addr));
        return cb;
    }
};

int epollfd;

// 封装了文件描述符的工具类
// 单线程 + 回调 + 异步非阻塞IO 实现高并发
// 只适用于网络这种IO密集型的场景下
// 相当于用户态模拟对线程的调度（实际上只有单线程），降低了多线程之间上下文切换的巨大开销
struct async_file{
    int m_fd;
    // callback<> m_resume;    //每个连接fd对应的回调函数，这样就不用维护回调函数的队列，事件触发是直接根据event.data.ptr定位回调函数执行即可

    // 工厂函数，用于创建 async_file 对象
    static async_file async_wrap(int fd){
        int flags = CHECK_CALL(fcntl,fd,F_GETFL);
        //将建立连接后的文件描述符增加非阻塞的属性
        flags |=O_NONBLOCK;
        CHECK_CALL(fcntl,fd,F_SETFL,flags);

        struct epoll_event event;
        event.events = EPOLLET;     //监听读取事件，设置边缘触发ET或者水平触发LT
        //  epoll control
        //  为连接注册epoll
        epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event); 

        return async_file{fd};
    }

    //同步读取方法,会阻塞
    ssize_t sync_read(bytes_view buf){
        return CHECK_CALL(read,m_fd,buf.data(),buf.size());
    }

    //异步读取方法，非阻塞
    void async_read(bytes_view buf,callback<ssize_t> cb){
        ssize_t ret = CHECK_CALL_EXCEPT(EAGAIN,read,m_fd,buf.data(),buf.size());
        if(ret!=-1){
            cb(ret);
            return ;
        }

        //如果read可以读了，请操作系统调用我的回调
        //回调函数绑定epoll
        callback<> resume = [this,buf,cb = std::move(cb)]() mutable{
            async_read(buf,std::move(cb));
        };

        struct epoll_event event;
        event.events = EPOLLIN | EPOLLET;    
        event.data.ptr = resume.leak_address();     //刻意让resume产生内存泄漏
        epoll_ctl(epollfd,EPOLL_CTL_MOD,m_fd,&event); 

    }

    //同步write方法，会阻塞
    ssize_t sync_write(bytes_const_view buf){
        ssize_t ret;
        do {
            //出现EAGAIN的错误，说明读取到的内容为空，需要再试一次而非阻塞
            ret = CHECK_CALL_EXCEPT(EAGAIN,write,m_fd,buf.data(),buf.size());

        }while(ret==-1);
        return ret;
    }

    //异步write操作
    void async_write(bytes_const_view buf,callback<ssize_t> cb){
        cb(CHECK_CALL(write,m_fd,buf.data(),buf.size()));
    }

    void close_file(){
        epoll_ctl(epollfd,EPOLL_CTL_DEL,m_fd,nullptr); 
        close(m_fd);
    }
};

struct http_connection_handler{
    async_file m_conn;
    bytes_buffer m_buf{1024};
    http_request_parser<> m_req_parser;

    void do_init(int connfd){
        m_conn = async_file::async_wrap(connfd);
        do_read();
    }

    void do_read(){
        fmt::print("开始读取...\n");
        m_conn.async_read(m_buf,[this](size_t n){
            //如果读到了EOF，说明对方关闭了http连接
            if(n==0){
                fmt::print("对面关闭连接\n");
                do_close();
                return;
            }
            //成功读取，则推入解析
            m_req_parser.push_chunk(m_buf.subspan(0,n));
            fmt::print("收到请求头: {}\n",m_req_parser.m_header_parser.headers_raw());
            fmt::print("收到请求正文: {}\n",m_req_parser.body());
            if(!m_req_parser.request_finished()){
                do_read();
            }else{
                do_write();
            }
        });
    }

    void do_write(){
        std::string_view body = std::move(m_req_parser.body());
        m_req_parser.reset_state();
        http_response_writer res_writer;
        
        if(body.empty()){
            body = "你好，你的请求正文为空";
        }else{
            body = fmt::format("你好，你的请求是：[{}],共{}字节",body,body.size());
        }

        res_writer.begin_header(200);
        res_writer.write_header("Server","co_http");
        res_writer.write_header("Content-type","text/html;charset=utf-8");
        res_writer.write_header("Connection","keep-alive");
        res_writer.write_header("Content-length",std::to_string(body.size()));
        res_writer.end_header();
        auto& buffer = res_writer.buffer();
        
        m_conn.sync_write(buffer);
        m_conn.sync_write(bytes_const_view{body.data(),body.size()});
        fmt::print("我的响应头: {}\n",buffer);
        fmt::print("我的响应正文: {}\n",body);

        do_read();
    }

    void do_close(){
        m_conn.close_file();
        delete this;
    }
};


void server(){
    
    address_resolver resolver;
    fmt::print("正在监听：127.0.0.1:8080\n");
    auto entry = resolver.resolve("127.0.0.1","8080");
    //  在客户端开始listen后，操作系统开始监听连接请求，维护一个连接队列（backlog）
    //  三次握手，服务端会把客户端的详细地址和端口放入连接队列中
    //  调用accept时，操作系统会从内核队列中取出一个已经完成握手的连接，将地址存入clientaddr中并返回一个新的fd作为文件描述符，完成握手开始通信
    int listenfd = entry.create_socket_and_bind();  //监听文件描述符，与连接文件描述法不同，connid才可以进行读写
    
    
    address_resolver::address client_addr;
    int connfd = CHECK_CALL(accept,listenfd,&client_addr.m_addr,&client_addr.m_addrlen);
    //必须按值(拷贝)进行捕获connid
    fmt::print("接受了一个连接: {}\n",connfd);

    //epollfd全局唯一
    epollfd = epoll_create1(0);

    http_connection_handler* conn_handler = new http_connection_handler{};
    conn_handler->do_init(connfd);

    struct epoll_event events[10];      //定义一个事件池

    while(true){
        int ret =  epoll_wait(epollfd,events,10,-1);   //events为监听的时间列表，10为最大监听数量，-1为最长监听时间（ms）,-1代表永远监听
        //返回值表示events的有效列表（events[0..ret-1]），负数则抛出异常
        if(ret < 0){
            throw;
        }
        for(int i=0;i<ret;i++){
            auto cb = callback<>::from_address(events[i].data.ptr);     //从泄漏内存中恢复对应的回调函数
            cb();
        }

    }
    fmt::print("所有任务都已完成\n");

    close(epollfd);
}


int main(){
    //TCP基于stream的协议，容易出现粘包的问题
    setlocale(LC_ALL,"zh_CN.UTF-8");    //将strerror的信息转换为中文
    try{
        server();
    }catch(std::exception const&e){
        fmt::print("错误: {}\n",e.what());
    }
    return 0;
} 