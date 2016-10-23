#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <syslog.h>
#include <unistd.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <piler.h>
#include "smtp.h"


void process_smtp_command(struct smtp_session *session, char *buf){
   char response[SMALLBUFSIZE];

   if(session->cfg->verbosity >= _LOG_DEBUG) syslog(LOG_PRIORITY, "processing command: *%s*", buf);

   if(strncasecmp(buf, SMTP_CMD_HELO, strlen(SMTP_CMD_HELO)) == 0){
      process_command_helo(session, response, sizeof(response));
      return;
   }

   if(strncasecmp(buf, SMTP_CMD_EHLO, strlen(SMTP_CMD_EHLO)) == 0 ||
         strncasecmp(buf, LMTP_CMD_LHLO, strlen(LMTP_CMD_LHLO)) == 0){
      process_command_ehlo_lhlo(session, response, sizeof(response));
      return;
   }

   if(strncasecmp(buf, SMTP_CMD_MAIL_FROM, strlen(SMTP_CMD_MAIL_FROM)) == 0){
      process_command_mail_from(session, buf);
      return;
   }

   if(strncasecmp(buf, SMTP_CMD_RCPT_TO, strlen(SMTP_CMD_RCPT_TO)) == 0){
      process_command_rcpt_to(session, buf);
      return;
   }

   if(strncasecmp(buf, SMTP_CMD_DATA, strlen(SMTP_CMD_DATA)) == 0){
      process_command_data(session);
      return;
   }

   if(session->cfg->enable_chunking == 1 && strncasecmp(buf, SMTP_CMD_BDAT, strlen(SMTP_CMD_BDAT)) == 0){
      get_bdat_size_to_read(session, buf);
      return;
   }

   if(strncasecmp(buf, SMTP_CMD_QUIT, strlen(SMTP_CMD_QUIT)) == 0){
      process_command_quit(session, response, sizeof(response));
      return;
   }

   if(strncasecmp(buf, SMTP_CMD_RESET, strlen(SMTP_CMD_RESET)) == 0){
      process_command_reset(session);
      return;
   }

   if(session->cfg->tls_enable == 1 && strncasecmp(buf, SMTP_CMD_STARTTLS, strlen(SMTP_CMD_STARTTLS)) == 0 && session->use_ssl == 0){
      process_command_starttls(session);
      return;
   }

   send_smtp_response(session, SMTP_RESP_502_ERR);
}


void process_data(struct smtp_session *session, char *readbuf, int readlen){
   char puf[SMALLBUFSIZE+BIGBUFSIZE];
   int n, pos, len;

   memset(puf, 0, sizeof(puf));

   if(session->buflen > 0){
      memcpy(&puf[0], session->buf, session->buflen);
      memcpy(&puf[session->buflen], readbuf, readlen);
      len = session->buflen + readlen;
   }
   else {
      memcpy(&puf[0], readbuf, readlen);
      len = readlen;
   }

   pos = searchStringInBuffer(&puf[0], len, SMTP_CMD_PERIOD, 5);

   if(pos > 0){
      //write(session->fd, puf, pos+2);
      //session->tot_len += pos+2;
      write(session->fd, puf, pos);
      session->tot_len += pos;
      process_command_period(session);
   }
   else {
      n = search_char_backward(&puf[0], len, '\r');

      if(n == -1 || len - n > 4){
         write(session->fd, puf, len);
         session->tot_len += len;
      }
      else {
         write(session->fd, puf, n);
         session->tot_len += n;

         snprintf(session->buf, SMALLBUFSIZE-1, "%s", &puf[n]);
         session->buflen = len - n;
      }
   }
}


void send_smtp_response(struct smtp_session *session, char *buf){
   int rc;
   char ssl_error[SMALLBUFSIZE];

   write1(session->socket, buf, strlen(buf), session->use_ssl, session->ssl);

   if(session->cfg->verbosity >= _LOG_DEBUG) syslog(LOG_PRIORITY, "sent: %s", buf);

   if(session->starttls == 1 && session->use_ssl == 0){

      if(session->cfg->verbosity >= _LOG_DEBUG) syslog(LOG_PRIORITY, "waiting for ssl handshake");

      rc = SSL_accept(session->ssl);

      if(session->cfg->verbosity >= _LOG_DEBUG) syslog(LOG_PRIORITY, "SSL_accept() finished");

      if(rc == 1){
         session->use_ssl = 1;
      }
      else {
         ERR_error_string_n(ERR_get_error(), ssl_error, SMALLBUFSIZE);
         syslog(LOG_PRIORITY, "%s: SSL_accept() failed, rc=%d, errorcode: %d, error text: %s\n", session->ttmpfile, rc, SSL_get_error(session->ssl, rc), ssl_error);
      }
   }
}


void process_command_helo(struct smtp_session *session, char *buf, int buflen){
   if(session->protocol_state == SMTP_STATE_INIT) session->protocol_state = SMTP_STATE_HELO;

   snprintf(buf, buflen-1, "220 %s ESMTP\r\n", session->cfg->hostid);
   send_smtp_response(session, buf);
}


void process_command_ehlo_lhlo(struct smtp_session *session, char *buf, int buflen){
   char extensions[SMALLBUFSIZE];

   memset(extensions, 0, sizeof(extensions));

   if(session->protocol_state == SMTP_STATE_INIT) session->protocol_state = SMTP_STATE_HELO;

   // if tls is not started, but it's enabled in the config
   if(session->use_ssl == 0 && session->cfg->tls_enable == 1) snprintf(extensions, sizeof(extensions)-1, "%s", SMTP_EXTENSION_STARTTLS);
   if(session->cfg->enable_chunking == 1) strncat(extensions, SMTP_EXTENSION_CHUNKING, sizeof(extensions)-strlen(extensions)-2);

   snprintf(buf, buflen-1, SMTP_RESP_250_EXTENSIONS, session->cfg->hostid, extensions);

   send_smtp_response(session, buf);
}


int init_ssl(struct smtp_session *session){
   session->ctx = SSL_CTX_new(TLSv1_server_method());

   if(session->ctx == NULL){
      syslog(LOG_PRIORITY, "%s: SSL ctx is null!", session->ttmpfile);
      return 0;
   }

   if(SSL_CTX_set_cipher_list(session->ctx, session->cfg->cipher_list) == 0){
      syslog(LOG_PRIORITY, "failed to set cipher list: '%s'", session->cfg->cipher_list);
      return 0;
   }

   if(SSL_CTX_use_PrivateKey_file(session->ctx, session->cfg->pemfile, SSL_FILETYPE_PEM) != 1){
      syslog(LOG_PRIORITY, "cannot load private key from %s", session->cfg->pemfile);
      return 0;
   }

   if(SSL_CTX_use_certificate_file(session->ctx, session->cfg->pemfile, SSL_FILETYPE_PEM) != 1){
      syslog(LOG_PRIORITY, "cannot load certificate from %s", session->cfg->pemfile);
      return 0;
   }

   return 1;
}


void process_command_starttls(struct smtp_session *session){
   if(session->cfg->verbosity >= _LOG_DEBUG) syslog(LOG_PRIORITY, "starttls request from client");

   if(init_ssl(session) == 1){

      session->ssl = SSL_new(session->ctx);
      if(session->ssl){

         SSL_set_options(session->ssl, SSL_OP_NO_SSLv2|SSL_OP_NO_SSLv3);

         if(SSL_set_fd(session->ssl, session->socket) == 1){
            session->starttls = 1;
            send_smtp_response(session, SMTP_RESP_220_READY_TO_START_TLS);
            session->protocol_state = SMTP_STATE_INIT;
            session->use_ssl = 1;
            return;
         } syslog(LOG_PRIORITY, "%s: SSL_set_fd() failed", session->ttmpfile);
      } syslog(LOG_PRIORITY, "%s: SSL_new() failed", session->ttmpfile);
   } syslog(LOG_PRIORITY, "%s: SSL ctx is null!", session->ttmpfile);

   send_smtp_response(session, SMTP_RESP_454_ERR_TLS_TEMP_ERROR);
}


void process_command_mail_from(struct smtp_session *session, char *buf){
   memset(session->mailfrom, 0, SMALLBUFSIZE);

   if(session->protocol_state != SMTP_STATE_HELO && session->protocol_state != SMTP_STATE_PERIOD && session->protocol_state != SMTP_STATE_BDAT){
      send(session->socket, SMTP_RESP_503_ERR, strlen(SMTP_RESP_503_ERR), 0);
   }
   else {
      create_id(&(session->ttmpfile[0]), 15);
      session->protocol_state = SMTP_STATE_MAIL_FROM;

      extractEmail(buf, session->mailfrom);

      reset_bdat_counters(session);
      session->tot_len = 0;

      send_smtp_response(session, SMTP_RESP_250_OK);
   }
}


void process_command_rcpt_to(struct smtp_session *session, char *buf){

   if(session->protocol_state == SMTP_STATE_MAIL_FROM || session->protocol_state == SMTP_STATE_RCPT_TO){

      // For now, we are not interested in the envelope recipients

      session->protocol_state = SMTP_STATE_RCPT_TO;
      send_smtp_response(session, SMTP_RESP_250_OK);
   }
   else {
      send_smtp_response(session, SMTP_RESP_503_ERR);
   }
}


void process_command_data(struct smtp_session *session){
   session->tot_len = 0;

   if(session->protocol_state != SMTP_STATE_RCPT_TO){
      send_smtp_response(session, SMTP_RESP_503_ERR);
   }
   else {
      session->fd = open(session->ttmpfile, O_CREAT|O_RDWR, S_IRUSR|S_IWUSR|S_IRGRP);
      if(session->fd == -1){
         syslog(LOG_PRIORITY, "%s: %s", ERR_OPEN_TMP_FILE, session->ttmpfile);
         send_smtp_response(session, SMTP_RESP_451_ERR);
      }
      else {
         session->protocol_state = SMTP_STATE_DATA;
         send_smtp_response(session, SMTP_RESP_354_DATA_OK);
      }
   }

}

void process_command_period(struct smtp_session *session){
   char buf[SMALLBUFSIZE];

   session->protocol_state = SMTP_STATE_PERIOD;

   // TODO: add some error handling

   fsync(session->fd);
   close(session->fd);

   session->fd = -1;

   syslog(LOG_PRIORITY, "received: %s, from=%s, size=%d", session->ttmpfile, session->mailfrom, session->tot_len);

   move_email(session);

   snprintf(buf, sizeof(buf)-1, "250 OK <%s>\r\n", session->ttmpfile);

   session->buflen = 0;
   memset(session->buf, 0, SMALLBUFSIZE);

   send_smtp_response(session, buf);
}


void process_command_quit(struct smtp_session *session, char *buf, int buflen){
   session->protocol_state = SMTP_STATE_FINISHED;

   snprintf(buf, buflen-1, SMTP_RESP_221_GOODBYE, session->cfg->hostid);

   send_smtp_response(session, buf);
}


void process_command_reset(struct smtp_session *session){
   send_smtp_response(session, SMTP_RESP_250_OK);

   session->tot_len = 0;
   session->fd = -1;
   session->protocol_state = SMTP_STATE_HELO;

   reset_bdat_counters(session);

   create_id(&(session->ttmpfile[0]), 15);
}

