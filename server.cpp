#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fmt/format.h>
#include <string.h>
#include <thread>
#include <vector>
#include <algorithm>
#include <map>

std::error_category &gai_category(){
    static struct final : std::error_category {
        const char* name() const noexcept override {
            return "getaddrinfo";
        }

        std::string message(int err) const override {
            return gai_strerror(err);
        }
    } instance;
    return instance;
}

template<int Expect = 0,class T>
T check_error(const char *msg, T res){
    if (res == -1) {
        if constexpr (Expect != 0) {
            if(errno == Expect){
                return -1;
            }
        }
        fmt::println("{}:{}", msg, std::strerror(errno));
        auto ec = std::error_code(errno,std::system_category());
        throw std::system_error(ec, msg);
    }
    return res;
}

#define SOURCE_INFO_IMPL(file,line) "In " file ":" #line ": "
#define SOURCE_INFO() SOURCE_INFO_IMPL(__FILE__,__LINE__)
#define CHECK_CALL_EXCEPT(expect, func, ...) check_error<expect>(SOURCE_INFO() #func,func(__VA_ARGS__))
#define CHECK_CALL(func, ...) check_error(#func,func(__VA_ARGS__))

struct address_resolver{
    struct socket_address_fatptr {
        struct sockaddr *ai_addr;
        socklen_t ai_addrlen;
    };

    struct socket_address_storage {
        union 
        {
            struct sockaddr m_addr;
            struct sockaddr_storage m_addr_storage;
        };
        socklen_t m_addrlen = sizeof( struct sockaddr_storage );

        operator socket_address_fatptr(){
            return {&m_addr,m_addrlen};
        }
    };

    struct address_resolved_entry {
        struct addrinfo *m_curr = nullptr;
        
        socket_address_fatptr get_address() const {
            return {m_curr->ai_addr,m_curr->ai_addrlen};
        }

        int creatsocket() const {
            int sockfd = check_error("socket",socket(m_curr->ai_family,m_curr->ai_socktype,m_curr->ai_protocol));
            return sockfd;
        }

        int creat_and_bind_socket() const {
            int sockfd = creatsocket();
            socket_address_fatptr serve_addr = get_address();
            
            CHECK_CALL(bind,sockfd,serve_addr.ai_addr,serve_addr.ai_addrlen);
            return sockfd;
        }

        [[nodiscard]] bool next_entry(){
            struct addrinfo *m_next = m_curr->ai_next;
            if( m_next == nullptr ){
                return false;
            }
            return true;
        }   
    };

    struct addrinfo *m_head = nullptr;

    void resolve(std::string const &name, std::string const &service){
        int err = getaddrinfo(name.c_str(),service.c_str(),NULL,&m_head);
        if ( err != 0 ) {
            fmt::println("getdaarinfo:{}{}",gai_strerror(err),err);
            auto ec = std::error_code(err, gai_category());
            throw std::system_error(ec, name + ":" + service);
        }
    }

    address_resolved_entry get_first_entry(){
        return {m_head};
    }

    address_resolver() = default;   
    
    address_resolver(address_resolver &&that) :m_head(that.m_head){
        that.m_head = nullptr;
    }

    ~address_resolver(){
        if(m_head){
            freeaddrinfo(m_head);
        }
    }
};

using StringMap = std::map<std::string,std::string>;

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

    bytes_const_view subspan(size_t start, size_t len) const {
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

struct bytes_buffer {
    std::vector<char> m_data;

    bytes_buffer() = default;

    explicit bytes_buffer(size_t n) : m_data(n) {}

    char const *data() const noexcept {
        return m_data.data();
    }

    char *data() noexcept {
        return m_data.data();
    }

    size_t size() const noexcept {
        return m_data.size();
    }

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

    bytes_const_view subspan(size_t start, size_t len) const {
        return operator bytes_const_view().subspan(start, len);
    }

    bytes_view subspan(size_t start, size_t len) {
        return operator bytes_view().subspan(start, len);
    }

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

    template <size_t N>
    void append_literial(const char (&literial)[N]) {
        append(std::string_view{literial, N - 1});
    }

    void resize(size_t n) {
        m_data.resize(n);
    }

    void reserve(size_t n) {
        m_data.reserve(n);
    }
};

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

struct http11_header_parser{
    std::string m_header;
    std::string m_body;
    std::string m_headline; //GET / HTTP/1.1
    StringMap m_header_keys;
    //size_t content_length = 0;
    bool m_header_finished = false;
    //bool m_body_finished = false;

    [[nodiscard]] bool header_parser_finished () {
        return m_header_finished;
    }
    
    //构建StringMap m_header_keys;
    //假装是私有函数
    void _exract_headers () {
        size_t pos = m_header.find("\r\n");
        m_headline = m_header.substr(0,pos);
        while( pos != m_header.npos){
            pos += 2;
            size_t nxt_pos = m_header.find("\r\n",pos);
            size_t line_len = std::string::npos;
            if( nxt_pos != std::string::npos){
                line_len = nxt_pos - pos;
            }

            std::string line = m_header.substr(pos,line_len);
            size_t col_pos = line.find(":");
            if( col_pos != line.npos ){
                std::string key = line.substr(0, col_pos);
                std::string value = line.substr(col_pos + 2);
                std::transform( key.begin(), key.end(), key.begin(), [](char c){
                    if('A' <= c && c <= 'Z'){
                        c += 'a'-'A';
                    }
                    return c;
                });
                
                m_header_keys.insert_or_assign(std::move(key),std::move(value));
            }
            pos = nxt_pos;
        }
    }

    void push_chunk(std::string_view chunk){
        if ( !m_header_finished ) {
            m_header.append(chunk);
            size_t m_header_len = m_header.find("\r\n\r\n");
            if(m_header_len != std::string::npos){
                m_header_finished = true;
                m_body = m_header.substr(m_header_len + 4); //多余的body存入m_body中
                m_header.resize(m_header_len);

                _exract_headers();//构建StringMap m_header_keys;
            }
        }else{
            m_body.append(chunk);
        }        
    }

    StringMap &headers () {
        return m_header_keys;
    }

    std::string &extra_body () { //解析头部过程中可能会带着一些多的body进来,body的主要解析过程放在struct _http_base_parser中实现
        return m_body;
    }

    std::string &header_raw () {
        return m_header;
    }

    std::string &headline (){
        return m_headline;
    }
};

template<class HeaderParser = http11_header_parser>
struct _http_base_parser{
    http11_header_parser m_header_parser;
    size_t m_content_length = 0;
    bool m_body_finished = false;

    [[nodiscard]] bool request_finished () {
        return m_body_finished;
    }

    size_t _extract_content_length () {
        auto &headers = m_header_parser.headers();
        auto it = headers.find("content-length");
        if( it == headers.end() ){
            return 0;
        }

        try{
            return std::stoi( it->second );
        } catch (std::invalid_argument const&){
            return 0;
        }

    }

    void push_chunks (std::string_view chunk) {
        if( !m_header_parser.header_parser_finished() ){
            m_header_parser.push_chunk(chunk);
            if( m_header_parser.header_parser_finished() ){
                m_content_length = _extract_content_length();
                std::string &body = m_header_parser.extra_body();
                if(body.size() >= m_content_length){
                    m_body_finished = true;
                    body.resize(m_content_length);
                }
            }
        }else{
            std::string &body = m_header_parser.extra_body();
            body.append(chunk);
            if(body.size() >= m_content_length){
                m_body_finished = true;
                body.resize(m_content_length);
            }
        }
    }

    std::string &body(){
        return m_header_parser.extra_body();
    }

    std::string &headers_raw(){
        return m_header_parser.header_raw();
    }

    std::string headerline_first(){
        auto &headline = m_header_parser.headline();
        auto space_pos = headline.find(" ");
        if( space_pos == std::string::npos ){
            return "";
        }
        return headline.substr(0, space_pos);
    }

    std::string headerline_second(){
        auto &headline = m_header_parser.headline();
        auto space1_pos = headline.find(" ");
        if( space1_pos == std::string::npos ){
            return "";
        }
        auto space2_pos = headline.find(" ");
        if( space2_pos == std::string::npos ){
            return "";
        }
        return headline.substr(space1_pos,space2_pos);
    }

    std::string headerline_third(){
        auto &headline = m_header_parser.headline();
        auto space1_pos = headline.find(" ");
        if( space1_pos == std::string::npos ){
            return "";
        }
        auto space2_pos = headline.find(" ");
        if( space2_pos == std::string::npos ){
            return "";
        }
        return headline.substr(space2_pos);
    }
};

template<class HeaderParser = http11_header_parser>
struct http_request_parser : _http_base_parser<HeaderParser> {
    std::string method() {
        return this->headerline_first();
    }

    std::string url() {
        return this->headerline_second();
    }

    std::string http_version() {
        return this->headerline_third();
    }
};

template<class HeaderParser = http11_header_parser>
struct http_response_parser : _http_base_parser<HeaderParser> {
    std::string http_version(){
        return this->headerline_first();
    }

    int status(){
        auto s = this->headerline_second();
        try{
            return std::stoi(s);
        } catch (std::invalid_argument const &e) {
            return -1;
        }
    }

    std::string status_string(){
        return this->headerline_third();
    }
};

struct http11_header_writer{
    std::string _buffer_to_write;

    void write_first (std::string const& first, std::string const& second,std::string const& third){
        _buffer_to_write.append(first);
        _buffer_to_write.append(" ");
        _buffer_to_write.append(second);
        _buffer_to_write.append(" ");
        _buffer_to_write.append(third);
    }

    void write_middle (std::string_view &key,std::string_view &value){
        _buffer_to_write.append("\r\n");
        _buffer_to_write.append(key);
        _buffer_to_write.append(":");
        _buffer_to_write.append(value);
    }

    void write_end() {
        _buffer_to_write.append("\r\n\r\n");
    }

    std::string &buffer(){
        return _buffer_to_write;
    }
};

template<class Header_writer = http11_header_writer>
struct request_writer{
    Header_writer m_request_writer;

    std::string &buffer(){
        return m_request_writer.buffer();
    }

    void begin_header(int status){
        m_request_writer.write_first("HTTP/1.1",std::to_string(status),"OK");
    }

    void write_header(std::string_view &key,std::string_view &value){
        m_request_writer.write_middle(key,value);
    }

    void end_header(){
        m_request_writer.write_end();
    }
};

template<class Header_writer = http11_header_writer>
struct response_writer{
    Header_writer m_response_writer;

    std::string &buffer(){
        return m_response_writer.buffer();
    }

    void begin_header(int status){
        m_response_writer.write_first("HTTP/1.1",std::to_string(status),"OK");
    }

    void write_header(std::string_view key,std::string_view value){
        m_response_writer.write_middle(key,value);
    }

    void end_header(){
        m_response_writer.write_end();
    }
};

std::vector<std::thread> pool;

template <class ...Args>
using callback = std::function<void(Args...)>;

struct async_file {
    int m_fd;

    static async_file async_wrap(int fd){
        int flags = CHECK_CALL(fcntl,fd,F_GETFL);
        flags |= O_NONBLOCK;
        CHECK_CALL(fcntl,fd,F_SETFL,flags);
        return async_file{fd};
    }

    ssize_t sync_read(bytes_view buf){
        ssize_t ret ; 
        do {
            ret = CHECK_CALL_EXCEPT(EAGAIN, read, m_fd, buf.data(), buf.size());
        }while(ret == -1);
        return ret;
    }

    void async_read(bytes_view buf, callback<ssize_t> cb){
        ssize_t ret ; 
        do {
            ret = CHECK_CALL_EXCEPT(EAGAIN, read, m_fd, buf.data(), buf.size());
        }while(ret == -1);
        cb(ret);
    }

    ssize_t sync_write(bytes_const_view buf) {
        return CHECK_CALL(write, m_fd, buf.data(), buf.size());
    }

    void async_write(bytes_const_view buf, callback<ssize_t> cb) {
        ssize_t ret = CHECK_CALL(write, m_fd, buf.data(), buf.size());
        cb(ret);
    }

};

struct http_connection_handler {
    async_file m_conn;
    bytes_buffer m_buffer{1024};
    http_request_parser<> m_req_parser;

    void do_init(int connfd) {
        m_conn = async_file::async_wrap(connfd);

        do_read();
    }

    void do_read() {
        fmt::println("开始读取...");
        m_conn.async_read(m_buffer, [this](size_t n) {
            if(n == 0) {
                fmt::println("收到对面关闭了连接");
                do_close();
                return;
            }

            fmt::println("收到了{}个字节:{}", n, m_buffer.data());
            m_req_parser.push_chunks(m_buffer.subspan(0,n));

            if( !m_req_parser.request_finished() ) {
                do_read();
            } else {
                do_write();
            }
        });
    }

    void do_write() {

        fmt::println("收到请求：{}",m_conn.m_fd);
        // fmt::println("收到请求:{}",request_parser.headers_raw());
        // fmt::println("收到请求正文:{}",request_parser.body());
        std::string &body = m_req_parser.body();

        // if( body.size() == 0 ){
        //     fmt::println("你好，你的请求正文为空");
        // }else{
        //     fmt::println("你好，你的请求是：[{}],共{}字节", body, body.size());
        // }

        response_writer res_writer;
        res_writer.begin_header(200);
        res_writer.write_header("Server","co_http");
        res_writer.write_header("Content-type","text/html;charset=utf-8");
        res_writer.write_header("Connection","keep-alive");
        res_writer.write_header("Content-length",std::to_string(body.size()));
        res_writer.end_header();
        auto &res = res_writer.buffer();
        //std::string res = "HTTP/1.1 200 OK\r\nServer: co_http\r\nConnection: close\r\nContent-length: "+ std::to_string(request_parser._extract_content_length()) +"\r\n\r\n" + body;
        // CHECK_CALL(write,m_conn.m_fd,res.data(),res.size());
        // CHECK_CALL(write,m_conn.m_fd,body.data(),body.size());

        m_conn.sync_write(bytes_const_view{res.data(),res.size()});
        m_conn.sync_write(bytes_const_view{body.data(),body.size()});

        // fmt::println("我的响应头:{}", res);
        // fmt::println("我的响应正文:{}", body);
        fmt::println("正在响应{}",m_conn.m_fd);

        do_read();
    }

    void do_close() {
        close(m_conn.m_fd);
    }

};

void server(){
    address_resolver my_resolver;
    my_resolver.resolve("127.0.0.1","8080");
    fmt::println("正在监听 127.0.0.1：8080");
    auto first_entry = my_resolver.get_first_entry();

    int listenfd = first_entry.creat_and_bind_socket();
    CHECK_CALL(listen,listenfd,SOMAXCONN);
    while (true) {
        address_resolver::socket_address_storage client_addr;
        int connid = CHECK_CALL(accept, listenfd, &client_addr.m_addr, &client_addr.m_addrlen);
        fmt::println("接受了一个连接：{}",connid);

        http_connection_handler conn_handler;
        conn_handler.do_init(connid);

        pool.push_back(std::thread ( [connid] {
            while(true){
                auto conn = async_file::async_wrap(connid);
                bytes_buffer buf(1024);            
                http_request_parser request_parser;
                do{
                    ssize_t n = conn.sync_read(buf);
                    if(n == 0){
                        fmt::println("收到对面关闭了连接：{}",connid);
                        goto quit;
                    }
                    request_parser.push_chunks(buf.subspan(0,n));

                }while ( !request_parser.request_finished() );
                fmt::println("收到请求：{}",connid);
                // fmt::println("收到请求:{}",request_parser.headers_raw());
                // fmt::println("收到请求正文:{}",request_parser.body());
                std::string &body = request_parser.body();

                // if( body.size() == 0 ){
                //     fmt::println("你好，你的请求正文为空");
                // }else{
                //     fmt::println("你好，你的请求是：[{}],共{}字节", body, body.size());
                // }

                response_writer res_writer;
                res_writer.begin_header(200);
                res_writer.write_header("Server","co_http");
                res_writer.write_header("Content-type","text/html;charset=utf-8");
                res_writer.write_header("Connection","keep-alive");
                res_writer.write_header("Content-length",std::to_string(body.size()));
                res_writer.end_header();
                auto &res = res_writer.buffer();
                //std::string res = "HTTP/1.1 200 OK\r\nServer: co_http\r\nConnection: close\r\nContent-length: "+ std::to_string(request_parser._extract_content_length()) +"\r\n\r\n" + body;
                CHECK_CALL(write,connid,res.data(),res.size());
                CHECK_CALL(write,connid,body.data(),body.size());

                // fmt::println("我的响应头:{}", res);
                // fmt::println("我的响应正文:{}", body);
                fmt::println("正在响应{}",connid);
            }
        quit:
            fmt::println("关闭连接{}",connid);
            close(connid);
        }));
    }
}

int main() {
    //setlocale(LC_ALL,"zh_CN.UTF-8");

    try{
        server();
    }catch (std::exception &e){
        fmt::println("错误：{}",e.what());
    }  

    for(auto &t: pool){
        t.join();
    }
    
    return 0;
}