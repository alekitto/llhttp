#include <stdio.h>
#include <string.h>

#ifndef LLHTTP__TEST
# include "llhttp.h"
#else
# define llhttp_t llparse_t
#endif  /* */

int llhttp_message_needs_eof(const llhttp_t* parser);
int llhttp_should_keep_alive(const llhttp_t* parser);
int llhttp__on_message_begin(const llhttp_t* parser, const char* p, const char* endp);
int llhttp__on_method(const llhttp_t* parser, const char* p, const char* endp);
int llhttp__on_protocol(const llhttp_t* parser, const char* p, const char* endp);

const char* HTTP_CONNECT = "CONNECT";

int llhttp__before_headers_complete(llhttp_t* parser, const char* p,
                                    const char* endp) {
  /* Set this here so that on_headers_complete() callbacks can see it */
  if ((parser->flags & F_UPGRADE) &&
      (parser->flags & F_CONNECTION_UPGRADE)) {
    /* For responses, "Upgrade: foo" and "Connection: upgrade" are
     * mandatory only when it is a 101 Switching Protocols response,
     * otherwise it is purely informational, to announce support.
     */
    parser->upgrade =
        (parser->type == HTTP_REQUEST || parser->status_code == 101);
  } else {
    parser->upgrade = F_METHOD_CONNECT == (parser->flags & F_METHOD_CONNECT);
  }
  return 0;
}


/* Return values:
 * 0 - No body, `restart`, message_complete
 * 1 - CONNECT request, `restart`, message_complete, and pause
 * 2 - chunk_size_start
 * 3 - body_identity
 * 4 - body_identity_eof
 * 5 - invalid transfer-encoding for request
 */
int llhttp__after_headers_complete(llhttp_t* parser, const char* p,
                                   const char* endp) {
  int hasBody;

  hasBody = parser->flags & F_CHUNKED || parser->content_length > 0;
  if (parser->upgrade && (F_METHOD_CONNECT == (parser->flags & F_METHOD_CONNECT) ||
                          (parser->flags & F_SKIPBODY) || !hasBody)) {
    /* Exit, the rest of the message is in a different protocol. */
    return 1;
  }

  if (parser->flags & F_SKIPBODY) {
    return 0;
  } else if (parser->flags & F_CHUNKED) {
    /* chunked encoding - ignore Content-Length header, prepare for a chunk */
    return 2;
  } else if (parser->flags & F_TRANSFER_ENCODING) {
    if (parser->type == HTTP_REQUEST && (parser->flags & F_LENIENT) == 0) {
      /* RFC 7230 3.3.3 */

      /* If a Transfer-Encoding header field
       * is present in a request and the chunked transfer coding is not
       * the final encoding, the message body length cannot be determined
       * reliably; the server MUST respond with the 400 (Bad Request)
       * status code and then close the connection.
       */
      return 5;
    } else {
      /* RFC 7230 3.3.3 */

      /* If a Transfer-Encoding header field is present in a response and
       * the chunked transfer coding is not the final encoding, the
       * message body length is determined by reading the connection until
       * it is closed by the server.
       */
      return 4;
    }
  } else {
    if (!(parser->flags & F_CONTENT_LENGTH)) {
      if (!llhttp_message_needs_eof(parser)) {
        /* Assume content-length 0 - read the next */
        return 0;
      } else {
        /* Read body until EOF */
        return 4;
      }
    } else if (parser->content_length == 0) {
      /* Content-Length header given but zero: Content-Length: 0\r\n */
      return 0;
    } else {
      /* Content-Length header given and non-zero */
      return 3;
    }
  }
}


int llhttp__after_message_complete(llhttp_t* parser, const char* p,
                                   const char* endp) {
  int should_keep_alive;

  should_keep_alive = llhttp_should_keep_alive(parser);
  parser->finish = HTTP_FINISH_SAFE;

  /* Keep `F_LENIENT` flag between messages, but reset every other flag */
  parser->flags &= F_LENIENT;

  /* NOTE: this is ignored in loose parsing mode */
  return should_keep_alive;
}


int llhttp_message_needs_eof(const llhttp_t* parser) {
  if (parser->type == HTTP_REQUEST) {
    return 0;
  }

  /* See RFC 2616 section 4.4 */
  if (parser->status_code / 100 == 1 || /* 1xx e.g. Continue */
      parser->status_code == 204 ||     /* No Content */
      parser->status_code == 304 ||     /* Not Modified */
      (parser->flags & F_SKIPBODY)) {     /* response to a HEAD request */
    return 0;
  }

  /* RFC 7230 3.3.3, see `llhttp__after_headers_complete` */
  if ((parser->flags & F_TRANSFER_ENCODING) &&
      (parser->flags & F_CHUNKED) == 0) {
    return 1;
  }

  if (parser->flags & (F_CHUNKED | F_CONTENT_LENGTH)) {
    return 0;
  }

  return 1;
}


int llhttp__internal_c_on_method(llhttp_t* parser, const char* p, const char* endp) {
  if (NULL == parser->method)
    parser->method = (void*) p;

  if (HTTP_BOTH == parser->type) {
    if (NULL == parser->method_or_protocol) {
      parser->method_or_protocol = (void*) p;
    }
    return 0;
  } else if (NULL != parser->method_or_protocol) {
    p = (const char*) parser->method_or_protocol;
    parser->method_or_protocol = NULL;
  }

  if (HTTP_RESPONSE == parser->type)
    return llhttp__on_protocol(parser, p, endp);

  parser->method_length = (uint16_t) (endp - (const char*)parser->method);
  if (7 == parser->method_length && 0 == memcmp(HTTP_CONNECT, parser->method, parser->method_length))
    parser->flags |= F_METHOD_CONNECT;
  else
    parser->flags &= ~F_METHOD_CONNECT;

  return llhttp__on_method(parser, p, endp);
}


int llhttp_should_keep_alive(const llhttp_t* parser) {
  if (parser->http_major > 0 && parser->http_minor > 0) {
    /* HTTP/1.1 */
    if (parser->flags & F_CONNECTION_CLOSE) {
      return 0;
    }
  } else {
    /* HTTP/1.0 or earlier */
    if (!(parser->flags & F_CONNECTION_KEEP_ALIVE)) {
      return 0;
    }
  }

  return !llhttp_message_needs_eof(parser);
}
