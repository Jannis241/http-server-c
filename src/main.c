#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netdb.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#define BACKLOG 128
#define DEFAULT_PORT "8080"
#define IO_BUFFER_SIZE 16384
#define MAX_HEADER_SIZE 16384
#define MAX_REQUEST_TARGET 8192
#define READ_TIMEOUT_SECONDS 15
#define SERVER_NAME "c-http-server/1.0"

typedef enum {
    METHOD_GET,
    METHOD_HEAD
} http_method;

typedef enum {
    READ_OK,
    READ_EMPTY,
    READ_TOO_LARGE,
    READ_TIMEOUT,
    READ_FAILED
} read_result;

typedef struct {
    const char *host;
    const char *port;
    const char *root_arg;
    char root[PATH_MAX];
} server_config;

typedef struct {
    http_method method;
    bool method_known;
    char target[MAX_REQUEST_TARGET];
    int http_major;
    int http_minor;
    bool host_present;
} http_request;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} byte_buffer;

static volatile sig_atomic_t g_stop = 0;

static void handle_stop_signal(int signum)
{
    (void)signum;
    g_stop = 1;
}

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [-b address] [-p port] [-r document-root]\n"
            "\n"
            "Options:\n"
            "  -b, --bind <address>  Bind address (default: all interfaces)\n"
            "  -p, --port <port>     TCP port or service name (default: 8080)\n"
            "  -r, --root <path>     Document root (default: current directory)\n"
            "  -h, --help            Show this help\n",
            program);
}

static void free_buffer(byte_buffer *buffer)
{
    free(buffer->data);
    buffer->data = NULL;
    buffer->len = 0;
    buffer->cap = 0;
}

static bool reserve_buffer(byte_buffer *buffer, size_t needed)
{
    size_t new_cap = buffer->cap == 0 ? 1024 : buffer->cap;
    char *new_data = NULL;

    if (needed <= buffer->cap) {
        return true;
    }

    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) {
            return false;
        }
        new_cap *= 2;
    }

    new_data = realloc(buffer->data, new_cap);
    if (new_data == NULL) {
        return false;
    }

    buffer->data = new_data;
    buffer->cap = new_cap;
    return true;
}

static bool append_bytes(byte_buffer *buffer, const char *data, size_t len)
{
    if (len > SIZE_MAX - buffer->len - 1) {
        return false;
    }

    if (!reserve_buffer(buffer, buffer->len + len + 1)) {
        return false;
    }

    memcpy(buffer->data + buffer->len, data, len);
    buffer->len += len;
    buffer->data[buffer->len] = '\0';
    return true;
}

static bool append_cstr(byte_buffer *buffer, const char *text)
{
    return append_bytes(buffer, text, strlen(text));
}

static bool append_char(byte_buffer *buffer, char ch)
{
    return append_bytes(buffer, &ch, 1);
}

static bool append_format(byte_buffer *buffer, const char *format, ...)
{
    va_list args;
    va_list copy;
    int needed = 0;
    bool ok = false;

    va_start(args, format);
    va_copy(copy, args);
    needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);

    if (needed < 0) {
        va_end(args);
        return false;
    }

    if (!reserve_buffer(buffer, buffer->len + (size_t)needed + 1)) {
        va_end(args);
        return false;
    }

    ok = vsnprintf(buffer->data + buffer->len,
                  buffer->cap - buffer->len,
                  format,
                  args) == needed;
    va_end(args);

    if (!ok) {
        return false;
    }

    buffer->len += (size_t)needed;
    return true;
}

static const char *status_reason(int status)
{
    switch (status) {
    case 200:
        return "OK";
    case 301:
        return "Moved Permanently";
    case 400:
        return "Bad Request";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 408:
        return "Request Timeout";
    case 414:
        return "URI Too Long";
    case 431:
        return "Request Header Fields Too Large";
    case 500:
        return "Internal Server Error";
    case 505:
        return "HTTP Version Not Supported";
    default:
        return "HTTP Error";
    }
}

static const char *method_name(http_method method)
{
    switch (method) {
    case METHOD_GET:
        return "GET";
    case METHOD_HEAD:
        return "HEAD";
    }

    return "UNKNOWN";
}

static bool format_http_date(time_t timestamp, char *buffer, size_t size)
{
    struct tm tm;

    if (gmtime_r(&timestamp, &tm) == NULL) {
        return false;
    }

    return strftime(buffer, size, "%a, %d %b %Y %H:%M:%S GMT", &tm) > 0;
}

static bool send_all(int fd, const char *data, size_t len)
{
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }

        if (n == 0) {
            return false;
        }

        sent += (size_t)n;
    }

    return true;
}

static bool send_response_headers(int fd,
                                  int status,
                                  const char *content_type,
                                  uintmax_t content_length,
                                  const char *extra_headers,
                                  const char *last_modified)
{
    char date[64];
    byte_buffer headers = {0};
    bool ok = false;

    if (!format_http_date(time(NULL), date, sizeof(date))) {
        return false;
    }

    ok = append_format(&headers,
                       "HTTP/1.1 %d %s\r\n"
                       "Server: %s\r\n"
                       "Date: %s\r\n"
                       "Connection: close\r\n"
                       "Content-Length: %" PRIuMAX "\r\n",
                       status,
                       status_reason(status),
                       SERVER_NAME,
                       date,
                       content_length);

    if (ok && content_type != NULL) {
        ok = append_format(&headers, "Content-Type: %s\r\n", content_type);
    }

    if (ok && last_modified != NULL) {
        ok = append_format(&headers, "Last-Modified: %s\r\n", last_modified);
    }

    if (ok && extra_headers != NULL) {
        ok = append_cstr(&headers, extra_headers);
    }

    if (ok) {
        ok = append_cstr(&headers, "\r\n") &&
             send_all(fd, headers.data, headers.len);
    }

    free_buffer(&headers);
    return ok;
}

static bool append_html_escaped(byte_buffer *buffer, const char *text)
{
    const unsigned char *p = (const unsigned char *)text;

    while (*p != '\0') {
        switch (*p) {
        case '&':
            if (!append_cstr(buffer, "&amp;")) {
                return false;
            }
            break;
        case '<':
            if (!append_cstr(buffer, "&lt;")) {
                return false;
            }
            break;
        case '>':
            if (!append_cstr(buffer, "&gt;")) {
                return false;
            }
            break;
        case '"':
            if (!append_cstr(buffer, "&quot;")) {
                return false;
            }
            break;
        case '\'':
            if (!append_cstr(buffer, "&#39;")) {
                return false;
            }
            break;
        default:
            if ((*p < 32 && *p != '\t') || *p == 127) {
                if (!append_char(buffer, '?')) {
                    return false;
                }
            } else if (!append_char(buffer, (char)*p)) {
                return false;
            }
            break;
        }
        p++;
    }

    return true;
}

static bool is_url_unreserved(unsigned char ch)
{
    return isalnum(ch) || ch == '-' || ch == '.' || ch == '_' || ch == '~';
}

static bool append_percent_encoded_byte(byte_buffer *buffer, unsigned char ch)
{
    static const char hex[] = "0123456789ABCDEF";
    char encoded[3] = {'%', hex[ch >> 4], hex[ch & 0x0F]};

    return append_bytes(buffer, encoded, sizeof(encoded));
}

static bool append_url_encoded(byte_buffer *buffer, const char *text)
{
    const unsigned char *p = (const unsigned char *)text;

    while (*p != '\0') {
        if (is_url_unreserved(*p)) {
            if (!append_char(buffer, (char)*p)) {
                return false;
            }
        } else if (!append_percent_encoded_byte(buffer, *p)) {
            return false;
        }
        p++;
    }

    return true;
}

static bool append_url_path_encoded(byte_buffer *buffer, const char *path)
{
    const unsigned char *p = (const unsigned char *)path;
    bool previous_slash = false;

    if (*p != '/' && !append_char(buffer, '/')) {
        return false;
    }

    while (*p != '\0') {
        if (*p == '/') {
            if (!previous_slash && !append_char(buffer, '/')) {
                return false;
            }
            previous_slash = true;
        } else {
            previous_slash = false;
            if (is_url_unreserved(*p)) {
                if (!append_char(buffer, (char)*p)) {
                    return false;
                }
            } else if (!append_percent_encoded_byte(buffer, *p)) {
                return false;
            }
        }
        p++;
    }

    return buffer->len > 0 || append_char(buffer, '/');
}

static bool send_html_response(int fd,
                               http_method method,
                               int status,
                               const char *body,
                               const char *extra_headers)
{
    size_t body_len = strlen(body);
    bool ok = send_response_headers(fd,
                                    status,
                                    "text/html; charset=utf-8",
                                    (uintmax_t)body_len,
                                    extra_headers,
                                    NULL);

    if (!ok || method == METHOD_HEAD) {
        return ok;
    }

    return send_all(fd, body, body_len);
}

static bool send_error_response(int fd,
                                http_method method,
                                int status,
                                const char *detail,
                                const char *extra_headers)
{
    byte_buffer body = {0};
    const char *reason = status_reason(status);
    bool ok = append_format(&body,
                            "<!doctype html>\n"
                            "<html lang=\"en\">\n"
                            "<head><meta charset=\"utf-8\"><title>%d %s</title></head>\n"
                            "<body><h1>%d %s</h1><p>",
                            status,
                            reason,
                            status,
                            reason);

    if (ok) {
        ok = append_html_escaped(&body, detail == NULL ? reason : detail);
    }

    if (ok) {
        ok = append_cstr(&body, "</p></body>\n</html>\n");
    }

    if (ok) {
        ok = send_html_response(fd, method, status, body.data, extra_headers);
    }

    free_buffer(&body);
    return ok;
}

static bool send_redirect(int fd, http_method method, const char *location)
{
    byte_buffer extra = {0};
    bool ok = append_format(&extra, "Location: %s\r\n", location);

    if (ok) {
        ok = send_error_response(fd,
                                 method,
                                 301,
                                 "The requested directory has moved.",
                                 extra.data);
    }

    free_buffer(&extra);
    return ok;
}

static const char *mime_type_for_path(const char *path)
{
    const char *ext = strrchr(path, '.');

    if (ext == NULL) {
        return "application/octet-stream";
    }

    ext++;

    if (strcasecmp(ext, "html") == 0 || strcasecmp(ext, "htm") == 0) {
        return "text/html; charset=utf-8";
    }
    if (strcasecmp(ext, "css") == 0) {
        return "text/css; charset=utf-8";
    }
    if (strcasecmp(ext, "js") == 0 || strcasecmp(ext, "mjs") == 0) {
        return "text/javascript; charset=utf-8";
    }
    if (strcasecmp(ext, "json") == 0) {
        return "application/json; charset=utf-8";
    }
    if (strcasecmp(ext, "txt") == 0 || strcasecmp(ext, "log") == 0) {
        return "text/plain; charset=utf-8";
    }
    if (strcasecmp(ext, "csv") == 0) {
        return "text/csv; charset=utf-8";
    }
    if (strcasecmp(ext, "xml") == 0) {
        return "application/xml; charset=utf-8";
    }
    if (strcasecmp(ext, "svg") == 0) {
        return "image/svg+xml";
    }
    if (strcasecmp(ext, "png") == 0) {
        return "image/png";
    }
    if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0) {
        return "image/jpeg";
    }
    if (strcasecmp(ext, "gif") == 0) {
        return "image/gif";
    }
    if (strcasecmp(ext, "webp") == 0) {
        return "image/webp";
    }
    if (strcasecmp(ext, "ico") == 0) {
        return "image/x-icon";
    }
    if (strcasecmp(ext, "pdf") == 0) {
        return "application/pdf";
    }
    if (strcasecmp(ext, "wasm") == 0) {
        return "application/wasm";
    }
    if (strcasecmp(ext, "mp4") == 0) {
        return "video/mp4";
    }
    if (strcasecmp(ext, "webm") == 0) {
        return "video/webm";
    }
    if (strcasecmp(ext, "mp3") == 0) {
        return "audio/mpeg";
    }
    if (strcasecmp(ext, "wav") == 0) {
        return "audio/wav";
    }

    return "application/octet-stream";
}

static int status_from_errno(int err)
{
    switch (err) {
    case EACCES:
    case EPERM:
        return 403;
    case ENOENT:
    case ENOTDIR:
        return 404;
    case ENAMETOOLONG:
        return 414;
    default:
        return 500;
    }
}

static bool is_path_under_root(const char *root, const char *path)
{
    size_t root_len = strlen(root);

    if (strcmp(root, "/") == 0) {
        return path[0] == '/';
    }

    return strcmp(root, path) == 0 ||
           (strncmp(root, path, root_len) == 0 && path[root_len] == '/');
}

static bool make_filesystem_path(const char *root,
                                 const char *url_path,
                                 char *out,
                                 size_t out_size)
{
    int written = 0;

    if (strcmp(root, "/") == 0) {
        written = snprintf(out, out_size, "%s", url_path);
    } else {
        written = snprintf(out, out_size, "%s%s", root, url_path);
    }

    return written >= 0 && (size_t)written < out_size;
}

static bool resolve_existing_path(const char *root,
                                  const char *candidate,
                                  char *resolved,
                                  size_t resolved_size,
                                  int *status)
{
    if (resolved_size < PATH_MAX) {
        *status = 500;
        return false;
    }

    if (realpath(candidate, resolved) == NULL) {
        *status = status_from_errno(errno);
        return false;
    }

    if (!is_path_under_root(root, resolved)) {
        *status = 403;
        return false;
    }

    return true;
}

static bool serve_file(int fd, http_method method, const char *path)
{
    struct stat st;
    char last_modified[64];
    const char *content_type = mime_type_for_path(path);
    int file_fd = open(path, O_RDONLY | O_CLOEXEC);
    bool ok = false;

    if (file_fd < 0) {
        return send_error_response(fd,
                                   method,
                                   status_from_errno(errno),
                                   "The file could not be opened.",
                                   NULL);
    }

    if (fstat(file_fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(file_fd);
        return send_error_response(fd,
                                   method,
                                   403,
                                   "The requested path is not a regular file.",
                                   NULL);
    }

    if (st.st_size < 0) {
        close(file_fd);
        return send_error_response(fd,
                                   method,
                                   500,
                                   "The file size could not be represented.",
                                   NULL);
    }

    if (!format_http_date(st.st_mtime, last_modified, sizeof(last_modified))) {
        last_modified[0] = '\0';
    }

    ok = send_response_headers(fd,
                               200,
                               content_type,
                               (uintmax_t)st.st_size,
                               NULL,
                               last_modified[0] == '\0' ? NULL : last_modified);

    if (ok && method != METHOD_HEAD) {
        char buffer[IO_BUFFER_SIZE];

        for (;;) {
            ssize_t n = read(file_fd, buffer, sizeof(buffer));
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                ok = false;
                break;
            }

            if (n == 0) {
                break;
            }

            if (!send_all(fd, buffer, (size_t)n)) {
                ok = false;
                break;
            }
        }
    }

    close(file_fd);
    return ok;
}

static bool append_directory_row(byte_buffer *body,
                                 const char *dir_path,
                                 const char *name)
{
    char child_path[PATH_MAX];
    char modified[64];
    struct stat st;
    int written = snprintf(child_path, sizeof(child_path), "%s/%s", dir_path, name);
    bool is_dir = false;

    if (written < 0 || (size_t)written >= sizeof(child_path)) {
        return true;
    }

    if (stat(child_path, &st) < 0) {
        return true;
    }

    is_dir = S_ISDIR(st.st_mode);
    if (!format_http_date(st.st_mtime, modified, sizeof(modified))) {
        strcpy(modified, "-");
    }

    if (!append_cstr(body, "<tr><td><a href=\"")) {
        return false;
    }

    if (!append_url_encoded(body, name)) {
        return false;
    }

    if (is_dir && !append_char(body, '/')) {
        return false;
    }

    if (!append_cstr(body, "\">")) {
        return false;
    }

    if (!append_html_escaped(body, name)) {
        return false;
    }

    if (is_dir && !append_char(body, '/')) {
        return false;
    }

    if (!append_cstr(body, "</a></td><td>")) {
        return false;
    }

    if (!append_html_escaped(body, modified)) {
        return false;
    }

    if (!append_cstr(body, "</td><td style=\"text-align:right\">")) {
        return false;
    }

    if (is_dir) {
        if (!append_cstr(body, "-")) {
            return false;
        }
    } else if (!append_format(body, "%" PRIuMAX, (uintmax_t)st.st_size)) {
        return false;
    }

    return append_cstr(body, "</td></tr>\n");
}

static bool serve_directory_listing(int fd,
                                    http_method method,
                                    const char *dir_path,
                                    const char *url_path)
{
    DIR *dir = opendir(dir_path);
    byte_buffer body = {0};
    struct dirent *entry = NULL;
    bool ok = false;

    if (dir == NULL) {
        return send_error_response(fd,
                                   method,
                                   status_from_errno(errno),
                                   "The directory could not be opened.",
                                   NULL);
    }

    ok = append_cstr(&body,
                     "<!doctype html>\n"
                     "<html lang=\"en\">\n"
                     "<head><meta charset=\"utf-8\">");
    if (ok) {
        ok = append_cstr(&body, "<title>Index of ");
    }
    if (ok) {
        ok = append_html_escaped(&body, url_path);
    }
    if (ok) {
        ok = append_cstr(&body,
                         "</title>"
                         "<style>"
                         "body{font-family:system-ui,sans-serif;margin:2rem;line-height:1.4}"
                         "table{border-collapse:collapse;min-width:36rem}"
                         "td,th{border-bottom:1px solid #ddd;padding:.35rem .6rem;text-align:left}"
                         "a{color:#0645ad;text-decoration:none}"
                         "a:hover{text-decoration:underline}"
                         "</style></head><body><h1>Index of ");
    }
    if (ok) {
        ok = append_html_escaped(&body, url_path);
    }
    if (ok) {
        ok = append_cstr(&body,
                         "</h1><table><thead><tr><th>Name</th><th>Modified</th>"
                         "<th>Size</th></tr></thead><tbody>\n");
    }

    if (ok && strcmp(url_path, "/") != 0) {
        ok = append_cstr(&body,
                         "<tr><td><a href=\"../\">../</a></td>"
                         "<td>-</td><td style=\"text-align:right\">-</td></tr>\n");
    }

    errno = 0;
    while (ok && (entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        ok = append_directory_row(&body, dir_path, entry->d_name);
    }

    if (ok && errno != 0) {
        ok = false;
    }

    if (ok) {
        ok = append_cstr(&body, "</tbody></table></body>\n</html>\n");
    }

    if (ok) {
        ok = send_response_headers(fd,
                                   200,
                                   "text/html; charset=utf-8",
                                   (uintmax_t)body.len,
                                   NULL,
                                   NULL);
    }

    if (ok && method != METHOD_HEAD) {
        ok = send_all(fd, body.data, body.len);
    }

    closedir(dir);
    free_buffer(&body);

    if (!ok) {
        return send_error_response(fd,
                                   method,
                                   500,
                                   "The directory listing could not be generated.",
                                   NULL);
    }

    return true;
}

static bool try_index_file(const char *root,
                           const char *dir_path,
                           const char *name,
                           char *resolved,
                           size_t resolved_size,
                           bool *found,
                           int *status)
{
    char candidate[PATH_MAX];
    struct stat st;
    int written = snprintf(candidate, sizeof(candidate), "%s/%s", dir_path, name);

    *found = false;

    if (written < 0 || (size_t)written >= sizeof(candidate)) {
        *status = 414;
        return false;
    }

    if (!resolve_existing_path(root, candidate, resolved, resolved_size, status)) {
        if (*status == 404) {
            return true;
        }
        return false;
    }

    if (stat(resolved, &st) < 0) {
        *status = status_from_errno(errno);
        return false;
    }

    if (S_ISREG(st.st_mode)) {
        *found = true;
    }

    return true;
}

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static bool has_control_or_space(const char *text)
{
    const unsigned char *p = (const unsigned char *)text;

    while (*p != '\0') {
        if (*p <= 32 || *p == 127) {
            return true;
        }
        p++;
    }

    return false;
}

static bool decode_url_path(const char *raw_path,
                            char *decoded,
                            size_t decoded_size,
                            int *status)
{
    size_t out = 0;

    for (size_t i = 0; raw_path[i] != '\0'; i++) {
        unsigned char ch = (unsigned char)raw_path[i];

        if (ch == '%') {
            int high = hex_value(raw_path[i + 1]);
            int low = hex_value(raw_path[i + 2]);

            if (high < 0 || low < 0) {
                *status = 400;
                return false;
            }

            ch = (unsigned char)((high << 4) | low);
            i += 2;

            if (ch == '\0') {
                *status = 400;
                return false;
            }
        }

        if (out + 1 >= decoded_size) {
            *status = 414;
            return false;
        }

        decoded[out++] = (char)ch;
    }

    decoded[out] = '\0';
    return true;
}

static bool contains_parent_segment(const char *path)
{
    const char *p = path;

    while (*p != '\0') {
        const char *start = NULL;
        size_t len = 0;

        while (*p == '/') {
            p++;
        }

        start = p;
        while (*p != '\0' && *p != '/') {
            p++;
        }

        len = (size_t)(p - start);
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            return true;
        }
    }

    return false;
}

static bool starts_with_scheme(const char *target, const char *scheme)
{
    size_t len = strlen(scheme);

    return strncasecmp(target, scheme, len) == 0;
}

static bool extract_request_paths(const char *target,
                                  char *decoded_path,
                                  size_t decoded_size,
                                  int *status)
{
    char raw_path[MAX_REQUEST_TARGET];
    const char *path_start = target;
    const char *path_end = NULL;
    size_t raw_len = 0;

    if (has_control_or_space(target)) {
        *status = 400;
        return false;
    }

    if (starts_with_scheme(target, "http://") ||
        starts_with_scheme(target, "https://")) {
        const char *authority = strstr(target, "://");
        authority += 3;
        path_start = strpbrk(authority, "/?#");

        if (path_start == NULL || *path_start != '/') {
            path_start = "/";
            path_end = path_start + 1;
        }
    } else if (target[0] != '/') {
        *status = 400;
        return false;
    }

    if (path_end == NULL) {
        path_end = strpbrk(path_start, "?#");
        if (path_end == NULL) {
            path_end = path_start + strlen(path_start);
        }
    }

    raw_len = (size_t)(path_end - path_start);
    if (raw_len == 0) {
        path_start = "/";
        raw_len = 1;
    }

    if (raw_len >= sizeof(raw_path)) {
        *status = 414;
        return false;
    }

    memcpy(raw_path, path_start, raw_len);
    raw_path[raw_len] = '\0';

    if (!decode_url_path(raw_path, decoded_path, decoded_size, status)) {
        return false;
    }

    if (decoded_path[0] != '/' || contains_parent_segment(decoded_path)) {
        *status = 403;
        return false;
    }

    return true;
}

static bool redirect_location_for_directory(const char *decoded_path,
                                            char *location,
                                            size_t location_size)
{
    byte_buffer encoded = {0};
    bool ok = append_url_path_encoded(&encoded, decoded_path);

    if (ok && (encoded.len == 0 || encoded.data[encoded.len - 1] != '/')) {
        ok = append_char(&encoded, '/');
    }

    if (ok && encoded.len >= location_size) {
        ok = false;
    }

    if (ok) {
        memcpy(location, encoded.data, encoded.len + 1);
    }

    free_buffer(&encoded);
    return ok;
}

static int handle_static_request(int fd,
                                 const server_config *config,
                                 const http_request *request)
{
    char decoded_path[MAX_REQUEST_TARGET];
    char candidate[PATH_MAX];
    char resolved[PATH_MAX];
    struct stat st;
    int status = 200;

    if (!extract_request_paths(request->target,
                               decoded_path,
                               sizeof(decoded_path),
                               &status)) {
        send_error_response(fd,
                            request->method,
                            status,
                            "The request target is invalid.",
                            NULL);
        return status;
    }

    if (!make_filesystem_path(config->root,
                              decoded_path,
                              candidate,
                              sizeof(candidate))) {
        send_error_response(fd,
                            request->method,
                            414,
                            "The request target is too long.",
                            NULL);
        return 414;
    }

    if (!resolve_existing_path(config->root,
                               candidate,
                               resolved,
                               sizeof(resolved),
                               &status)) {
        send_error_response(fd,
                            request->method,
                            status,
                            "The requested resource could not be found.",
                            NULL);
        return status;
    }

    if (stat(resolved, &st) < 0) {
        status = status_from_errno(errno);
        send_error_response(fd,
                            request->method,
                            status,
                            "The requested resource could not be inspected.",
                            NULL);
        return status;
    }

    if (S_ISDIR(st.st_mode)) {
        bool found_index = false;
        char index_path[PATH_MAX];

        if (decoded_path[strlen(decoded_path) - 1] != '/') {
            char location[MAX_REQUEST_TARGET + 2];

            if (!redirect_location_for_directory(decoded_path,
                                                 location,
                                                 sizeof(location))) {
                send_error_response(fd,
                                    request->method,
                                    414,
                                    "The redirect target is too long.",
                                    NULL);
                return 414;
            }

            send_redirect(fd, request->method, location);
            return 301;
        }

        if (!try_index_file(config->root,
                            resolved,
                            "index.html",
                            index_path,
                            sizeof(index_path),
                            &found_index,
                            &status)) {
            send_error_response(fd,
                                request->method,
                                status,
                                "The directory index could not be opened.",
                                NULL);
            return status;
        }

        if (!found_index &&
            !try_index_file(config->root,
                            resolved,
                            "index.htm",
                            index_path,
                            sizeof(index_path),
                            &found_index,
                            &status)) {
            send_error_response(fd,
                                request->method,
                                status,
                                "The directory index could not be opened.",
                                NULL);
            return status;
        }

        if (found_index) {
            if (!serve_file(fd, request->method, index_path)) {
                return 500;
            }
            return 200;
        }

        if (!serve_directory_listing(fd, request->method, resolved, decoded_path)) {
            return 500;
        }
        return 200;
    }

    if (!S_ISREG(st.st_mode)) {
        send_error_response(fd,
                            request->method,
                            403,
                            "The requested resource is not a regular file.",
                            NULL);
        return 403;
    }

    if (!serve_file(fd, request->method, resolved)) {
        return 500;
    }

    return 200;
}

static char *trim_ascii_space(char *text)
{
    char *end = NULL;

    while (*text != '\0' && isspace((unsigned char)*text)) {
        text++;
    }

    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';

    return text;
}

static bool header_value_has_token(const char *value, const char *token)
{
    size_t token_len = strlen(token);
    const char *p = value;

    while (*p != '\0') {
        const char *start = NULL;
        const char *end = NULL;

        while (*p == ',' || isspace((unsigned char)*p)) {
            p++;
        }

        start = p;
        while (*p != '\0' && *p != ',') {
            p++;
        }
        end = p;

        while (end > start && isspace((unsigned char)end[-1])) {
            end--;
        }

        if ((size_t)(end - start) == token_len &&
            strncasecmp(start, token, token_len) == 0) {
            return true;
        }
    }

    return false;
}

static int parse_http_version(const char *version, int *major, int *minor)
{
    char extra = '\0';

    if (sscanf(version, "HTTP/%d.%d%c", major, minor, &extra) != 2) {
        return 400;
    }

    if (*major != 1 || (*minor != 0 && *minor != 1)) {
        return 505;
    }

    return 0;
}

static int parse_request_line(char *line, http_request *request)
{
    char *method = line;
    char *target = strchr(method, ' ');
    char *version = NULL;
    int status = 0;

    if (target == NULL) {
        return 400;
    }

    *target = '\0';
    target++;

    while (*target == ' ') {
        target++;
    }

    version = strchr(target, ' ');
    if (version == NULL) {
        return 400;
    }

    *version = '\0';
    version++;

    while (*version == ' ') {
        version++;
    }

    if (*method == '\0' || *target == '\0' || *version == '\0' ||
        strchr(version, ' ') != NULL) {
        return 400;
    }

    if (strcmp(method, "GET") == 0) {
        request->method = METHOD_GET;
        request->method_known = true;
    } else if (strcmp(method, "HEAD") == 0) {
        request->method = METHOD_HEAD;
        request->method_known = true;
    } else {
        request->method = METHOD_GET;
        request->method_known = false;
        return 405;
    }

    if (strlen(target) >= sizeof(request->target)) {
        return 414;
    }
    strcpy(request->target, target);

    status = parse_http_version(version, &request->http_major, &request->http_minor);
    if (status != 0) {
        return status;
    }

    return 0;
}

static int parse_headers(char *headers, http_request *request)
{
    char *line = headers;

    while (*line != '\0') {
        char *next = strchr(line, '\n');
        char *colon = NULL;
        char *name = NULL;
        char *value = NULL;

        if (next != NULL) {
            *next = '\0';
            next++;
        }

        if (*line != '\0' && line[strlen(line) - 1] == '\r') {
            line[strlen(line) - 1] = '\0';
        }

        if (*line == '\0') {
            break;
        }

        if (*line == ' ' || *line == '\t') {
            return 400;
        }

        colon = strchr(line, ':');
        if (colon == NULL) {
            return 400;
        }

        *colon = '\0';
        name = trim_ascii_space(line);
        value = trim_ascii_space(colon + 1);

        if (*name == '\0') {
            return 400;
        }

        if (strcasecmp(name, "Host") == 0 && *value != '\0') {
            request->host_present = true;
        }

        if (strcasecmp(name, "Connection") == 0 &&
            header_value_has_token(value, "close")) {
            /* The server always closes responses, but parsing this validates the header. */
        }

        if (next == NULL) {
            break;
        }
        line = next;
    }

    if (request->http_major == 1 && request->http_minor == 1 &&
        !request->host_present) {
        return 400;
    }

    return 0;
}

static int parse_http_request(char *buffer, size_t len, http_request *request)
{
    char *headers = NULL;
    char *line_end = NULL;
    int status = 0;

    memset(request, 0, sizeof(*request));
    request->method = METHOD_GET;

    if (memchr(buffer, '\0', len) != NULL) {
        return 400;
    }

    buffer[len] = '\0';
    line_end = strchr(buffer, '\n');
    if (line_end == NULL) {
        return 400;
    }

    *line_end = '\0';
    headers = line_end + 1;

    if (line_end > buffer && line_end[-1] == '\r') {
        line_end[-1] = '\0';
    }

    status = parse_request_line(buffer, request);
    if (status != 0) {
        return status;
    }

    return parse_headers(headers, request);
}

static bool has_complete_header(const char *buffer, size_t len)
{
    for (size_t i = 0; i + 3 < len; i++) {
        if (buffer[i] == '\r' && buffer[i + 1] == '\n' &&
            buffer[i + 2] == '\r' && buffer[i + 3] == '\n') {
            return true;
        }
    }

    for (size_t i = 0; i + 1 < len; i++) {
        if (buffer[i] == '\n' && buffer[i + 1] == '\n') {
            return true;
        }
    }

    return false;
}

static read_result read_http_request(int fd, char *buffer, size_t size, size_t *len)
{
    *len = 0;

    while (*len < size - 1) {
        ssize_t n = recv(fd, buffer + *len, size - 1 - *len, 0);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return READ_TIMEOUT;
            }
            return READ_FAILED;
        }

        if (n == 0) {
            return *len == 0 ? READ_EMPTY : READ_FAILED;
        }

        *len += (size_t)n;
        if (has_complete_header(buffer, *len)) {
            buffer[*len] = '\0';
            return READ_OK;
        }
    }

    return READ_TOO_LARGE;
}

static bool set_client_timeouts(int fd)
{
    struct timeval timeout;

    timeout.tv_sec = READ_TIMEOUT_SECONDS;
    timeout.tv_usec = 0;

    return setsockopt(fd,
                      SOL_SOCKET,
                      SO_RCVTIMEO,
                      &timeout,
                      sizeof(timeout)) == 0;
}

static void handle_client(int client_fd, const server_config *config)
{
    char buffer[MAX_HEADER_SIZE + 1];
    size_t len = 0;
    read_result result = READ_FAILED;
    http_request request;
    int status = 0;

    (void)set_client_timeouts(client_fd);

    result = read_http_request(client_fd, buffer, sizeof(buffer), &len);
    if (result == READ_EMPTY) {
        return;
    }

    if (result == READ_TOO_LARGE) {
        send_error_response(client_fd,
                            METHOD_GET,
                            431,
                            "The request headers are too large.",
                            NULL);
        return;
    }

    if (result == READ_TIMEOUT) {
        send_error_response(client_fd,
                            METHOD_GET,
                            408,
                            "The request timed out before all headers arrived.",
                            NULL);
        return;
    }

    if (result != READ_OK) {
        send_error_response(client_fd,
                            METHOD_GET,
                            400,
                            "The request could not be read.",
                            NULL);
        return;
    }

    status = parse_http_request(buffer, len, &request);
    if (status != 0) {
        const char *extra = status == 405 ? "Allow: GET, HEAD\r\n" : NULL;
        http_method method = request.method_known ? request.method : METHOD_GET;

        send_error_response(client_fd,
                            method,
                            status,
                            "The request is not supported by this server.",
                            extra);
        fprintf(stderr, "- - -> %d\n", status);
        return;
    }

    status = handle_static_request(client_fd, config, &request);
    fprintf(stderr, "%s %s -> %d\n", method_name(request.method), request.target, status);
}

static bool install_signal_handlers(void)
{
    struct sigaction action;
    struct sigaction ignore;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_stop_signal;
    sigemptyset(&action.sa_mask);

    if (sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0) {
        return false;
    }

    memset(&ignore, 0, sizeof(ignore));
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);

    if (sigaction(SIGPIPE, &ignore, NULL) < 0 ||
        sigaction(SIGCHLD, &ignore, NULL) < 0) {
        return false;
    }

    return true;
}

static bool parse_args(int argc, char **argv, server_config *config)
{
    memset(config, 0, sizeof(*config));
    config->host = NULL;
    config->port = DEFAULT_PORT;
    config->root_arg = ".";

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char **target = NULL;

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage(argv[0]);
            exit(EXIT_SUCCESS);
        }

        if (strcmp(arg, "-b") == 0 || strcmp(arg, "--bind") == 0) {
            target = &config->host;
        } else if (strcmp(arg, "-p") == 0 || strcmp(arg, "--port") == 0) {
            target = &config->port;
        } else if (strcmp(arg, "-r") == 0 || strcmp(arg, "--root") == 0) {
            target = &config->root_arg;
        } else {
            fprintf(stderr, "Unknown option: %s\n", arg);
            return false;
        }

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for %s\n", arg);
            return false;
        }

        i++;
        *target = argv[i];
    }

    if (realpath(config->root_arg, config->root) == NULL) {
        fprintf(stderr, "Invalid document root '%s': %s\n",
                config->root_arg,
                strerror(errno));
        return false;
    }

    return true;
}

static bool validate_document_root(const char *root)
{
    struct stat st;

    if (stat(root, &st) < 0) {
        fprintf(stderr, "Could not stat document root '%s': %s\n",
                root,
                strerror(errno));
        return false;
    }

    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Document root is not a directory: %s\n", root);
        return false;
    }

    return true;
}

static int create_listener(const char *host, const char *port)
{
    struct addrinfo hints;
    struct addrinfo *results = NULL;
    struct addrinfo *item = NULL;
    int listener = -1;
    int last_errno = 0;
    int gai = 0;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    gai = getaddrinfo(host, port, &hints, &results);
    if (gai != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(gai));
        return -1;
    }

    for (item = results; item != NULL; item = item->ai_next) {
        int yes = 1;

        listener = socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if (listener < 0) {
            last_errno = errno;
            continue;
        }

        (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        if (bind(listener, item->ai_addr, item->ai_addrlen) == 0 &&
            listen(listener, BACKLOG) == 0) {
            break;
        }

        last_errno = errno;
        close(listener);
        listener = -1;
    }

    freeaddrinfo(results);

    if (listener < 0 && last_errno != 0) {
        fprintf(stderr, "Could not bind listener: %s\n", strerror(last_errno));
    }

    return listener;
}

static void serve_forever(int listener, const server_config *config)
{
    while (!g_stop) {
        struct sockaddr_storage address;
        socklen_t address_len = sizeof(address);
        int client_fd = accept(listener, (struct sockaddr *)&address, &address_len);
        pid_t child = -1;

        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            continue;
        }

        child = fork();
        if (child < 0) {
            perror("fork");
            handle_client(client_fd, config);
            close(client_fd);
            continue;
        }

        if (child == 0) {
            close(listener);
            handle_client(client_fd, config);
            close(client_fd);
            _exit(EXIT_SUCCESS);
        }

        close(client_fd);
    }
}

int main(int argc, char **argv)
{
    server_config config;
    int listener = -1;

    if (!parse_args(argc, argv, &config)) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (!validate_document_root(config.root)) {
        return EXIT_FAILURE;
    }

    if (!install_signal_handlers()) {
        perror("sigaction");
        return EXIT_FAILURE;
    }

    listener = create_listener(config.host, config.port);
    if (listener < 0) {
        return EXIT_FAILURE;
    }

    fprintf(stderr,
            "Serving %s on http://%s:%s/\n",
            config.root,
            config.host == NULL ? "0.0.0.0" : config.host,
            config.port);

    serve_forever(listener, &config);
    close(listener);

    return EXIT_SUCCESS;
}
