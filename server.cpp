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

int check_error(const char *msg,int res){
    if (res == -1) {
        fmt::println("{}:{}", msg, std::strerror(errno));
        throw;
    }
    return res;
}

ssize_t check_error(const char *msg,ssize_t res){
    if (res == -1) {
        fmt::println("{}:{}", msg, std::strerror(errno));
        throw;
    }
    return res;
}

#define CHECK_CALL(func, ...) check_error(#func,func(__VA_ARGS__))

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

struct address_resolver{
    struct addrinfo *m_head = nullptr;

    void resolve(std::string const &name, std::string const &service){
        int err = getaddrinfo(name.c_str(),service.c_str(),NULL,&m_head);
        if ( err != 0 ) {
            fmt::println("getdaarinfo:{}{}",gai_strerror(err),err);
            throw;
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

int main() {
    //setlocale(LC_ALL,"zh_CN.UTF-8");

    address_resolver my_resolver;
    my_resolver.resolve("127.0.0.1","9080");
    fmt::println("正在监听 127.0.0.1：9080");
    auto first_entry = my_resolver.get_first_entry();

    int listenfd = first_entry.creat_and_bind_socket();
    CHECK_CALL(listen,listenfd,SOMAXCONN);
    while (true) {
        socket_address_storage client_addr;
        int connid = CHECK_CALL(accept, listenfd, &client_addr.m_addr, &client_addr.m_addrlen);
        fmt::println("接受了一个连接：{}",connid);
        pool.push_back(std::thread ( [connid] {
        
            char buf[1024];            
            http_request_parser request_parser;
            do{
                ssize_t n = CHECK_CALL(read, connid, buf, sizeof(buf));
                request_parser.push_chunks(std::string_view(buf,n));
            }while ( !request_parser.request_finished() );
            fmt::println("收到请求：{}",connid);
            // fmt::println("收到请求:{}",request_parser.headers_raw());
            // fmt::println("收到请求正文:{}",request_parser.body());
            std::string &body = request_parser.body();

            if( body.size() == 0 ){
                fmt::println("你好，你的请求正文为空");
            }else{
                fmt::println("你好，你的请求是：[{}],共{}字节", body, body.size());
            }

            body = body + "给正文加点料";

            response_writer res_writer;
            res_writer.begin_header(200);
            res_writer.write_header("Server","co_http");
            res_writer.write_header("Content-type","text/html;charset=utf-8");
            res_writer.write_header("Connection","colse");
            res_writer.write_header("Content-length",std::to_string(body.size()));
            res_writer.end_header();
            auto &res = res_writer.buffer();
            //std::string res = "HTTP/1.1 200 OK\r\nServer: co_http\r\nConnection: close\r\nContent-length: "+ std::to_string(request_parser._extract_content_length()) +"\r\n\r\n" + body;
            CHECK_CALL(write,connid,res.data(),res.size());
            CHECK_CALL(write,connid,body.data(),body.size());

            fmt::println("我的响应头:{}", res);
            fmt::println("我的响应正文:{}", body);

            close(connid);
        }));
    }

    for(auto &t: pool){
        t.join();
    }
    
    return 0;
}