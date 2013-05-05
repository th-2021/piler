/*
 * mysql.c, SJ
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <piler.h>


int open_database(struct session_data *sdata, struct __config *cfg){
   int rc=1;

   mysql_init(&(sdata->mysql));

   mysql_options(&(sdata->mysql), MYSQL_OPT_CONNECT_TIMEOUT, (const char*)&cfg->mysql_connect_timeout);
   mysql_options(&(sdata->mysql), MYSQL_OPT_RECONNECT, (const char*)&rc);

   if(mysql_real_connect(&(sdata->mysql), cfg->mysqlhost, cfg->mysqluser, cfg->mysqlpwd, cfg->mysqldb, cfg->mysqlport, cfg->mysqlsocket, 0) == 0){
      printf("cant connect to mysql server\n");
      return ERR;
   }

   mysql_real_query(&(sdata->mysql), "SET NAMES utf8", strlen("SET NAMES utf8"));
   mysql_real_query(&(sdata->mysql), "SET CHARACTER SET utf8", strlen("SET CHARACTER SET utf8"));

   return OK;
}


void close_database(struct session_data *sdata){
   mysql_close(&(sdata->mysql));
}


void p_bind_init(struct __data *data){
   int i;

   data->pos = 0;

   for(i=0; i<MAX_SQL_VARS; i++){
      data->sql[i] = NULL;
      data->type[i] = TYPE_UNDEF;
      data->len[i] = 0;
   }
}


void p_query(struct session_data *sdata, char *s){
   mysql_real_query(&(sdata->mysql), s, strlen(s));
}


int p_exec_query(struct session_data *sdata, MYSQL_STMT *stmt, struct __data *data){
   MYSQL_BIND bind[MAX_SQL_VARS];
   unsigned long length[MAX_SQL_VARS];
   int i, ret=ERR;

   memset(bind, 0, sizeof(bind));

   for(i=0; i<MAX_SQL_VARS; i++){
      if(data->type[i] > TYPE_UNDEF){


         switch(data->type[i]) {
             case TYPE_TINY:
                                  bind[i].buffer_type = MYSQL_TYPE_TINY;
                                  bind[i].length = 0;
                                  break;

             case TYPE_LONG:
                                  bind[i].buffer_type = MYSQL_TYPE_LONG;
                                  bind[i].length = 0;
                                  break;


             case TYPE_LONGLONG:
                                  bind[i].buffer_type = MYSQL_TYPE_LONGLONG;
                                  bind[i].length = 0;
                                  break;


             case TYPE_STRING:
                                  bind[i].buffer_type = MYSQL_TYPE_STRING;
                                  length[i] = strlen(data->sql[i]);
                                  bind[i].length = &length[i];
                                  break;


             default:
                                  bind[i].buffer_type = 0;
                                  bind[i].length = 0;
                                  break;

         };


         bind[i].buffer = data->sql[i];
         bind[i].is_null = 0;

         
      }
      else { break; }
   }

   if(mysql_stmt_bind_param(stmt, bind)){
      syslog(LOG_PRIORITY, "%s: mysql_stmt_bind_param() error: %s", sdata->ttmpfile, mysql_stmt_error(stmt));
      goto CLOSE;
   }

   if(mysql_stmt_execute(stmt)){
      syslog(LOG_PRIORITY, "%s: mysql_stmt_execute error: *%s*", sdata->ttmpfile, mysql_error(&(sdata->mysql)));
      goto CLOSE;
   }

   ret = OK;

CLOSE:
   return ret;
}


int p_store_results(struct session_data *sdata, MYSQL_STMT *stmt, struct __data *data){
   MYSQL_BIND bind[MAX_SQL_VARS];
   my_bool is_null[MAX_SQL_VARS];
   unsigned long length[MAX_SQL_VARS];
   my_bool error[MAX_SQL_VARS];
   int i, ret=ERR;

   memset(bind, 0, sizeof(bind));

   for(i=0; i<MAX_SQL_VARS; i++){
      if(data->type[i] > TYPE_UNDEF){

         switch(data->type[i]) {
             case TYPE_TINY:      bind[i].buffer_type = MYSQL_TYPE_TINY;
                                  break;


             case TYPE_LONG:      bind[i].buffer_type = MYSQL_TYPE_LONG;
                                  break;


             case TYPE_LONGLONG:
                                  bind[i].buffer_type = MYSQL_TYPE_LONGLONG;
                                  break;


             case TYPE_STRING:
                                  bind[i].buffer_type = MYSQL_TYPE_STRING;
                                  bind[i].buffer_length = data->len[i];
                                  break;

             default:
                                  bind[i].buffer_type = 0;
                                  break;

         };


         bind[i].buffer = (char *)data->sql[i];
         bind[i].is_null = &is_null[i];
         bind[i].length = &length[i];
         bind[i].error = &error[i];

      }
      else { break; }
   }

   if(mysql_stmt_bind_result(stmt, bind)){
      goto CLOSE;
   }


   if(mysql_stmt_store_result(stmt)){
      goto CLOSE;
   }

   ret = OK;

CLOSE:

   return ret;
}


int p_fetch_results(MYSQL_STMT *stmt){

   if(mysql_stmt_fetch(stmt) == 0) return OK;

   return ERR;
}


void p_free_results(MYSQL_STMT *stmt){
   mysql_stmt_free_result(stmt);
}


uint64 p_get_insert_id(MYSQL_STMT *stmt){
   return mysql_stmt_insert_id(stmt);
}


int p_get_affected_rows(MYSQL_STMT *stmt){
   return mysql_stmt_affected_rows(stmt);
}


int prepare_sql_statement(struct session_data *sdata, MYSQL_STMT **stmt, char *s){

   *stmt = mysql_stmt_init(&(sdata->mysql));
   if(!*stmt){
      syslog(LOG_PRIORITY, "%s: mysql_stmt_init() error", sdata->ttmpfile);
      return ERR;
   }

   if(mysql_stmt_prepare(*stmt, s, strlen(s))){
      syslog(LOG_PRIORITY, "%s: mysql_stmt_prepare() error: %s => sql: %s", sdata->ttmpfile, mysql_stmt_error(*stmt), s);
      return ERR;
   }

   return OK; 
}


void insert_offset(struct session_data *sdata, int server_id){
   char s[SMALLBUFSIZE];
   uint64 id = server_id * 1000000000000ULL;

   snprintf(s, sizeof(s)-1, "INSERT INTO %s (`id`) VALUES (%llu)", SQL_METADATA_TABLE, id);

   mysql_real_query(&(sdata->mysql), s, strlen(s));
}


int create_prepared_statements(struct session_data *sdata, struct __data *data){
   int rc = OK;

   data->stmt_get_meta_id_by_message_id = NULL;
   data->stmt_insert_into_rcpt_table = NULL;
   data->stmt_update_metadata_reference = NULL;

   if(prepare_sql_statement(sdata, &(data->stmt_get_meta_id_by_message_id), SQL_PREPARED_STMT_GET_META_ID_BY_MESSAGE_ID) == ERR) rc = ERR;
   if(prepare_sql_statement(sdata, &(data->stmt_insert_into_rcpt_table), SQL_PREPARED_STMT_INSERT_INTO_RCPT_TABLE) == ERR) rc = ERR;
   if(prepare_sql_statement(sdata, &(data->stmt_update_metadata_reference), SQL_PREPARED_STMT_UPDATE_METADATA_REFERENCE) == ERR) rc = ERR;

   return rc;
}


void close_prepared_statement(MYSQL_STMT *stmt){
   if(stmt) mysql_stmt_close(stmt);
}


void close_prepared_statements(struct __data *data){
   if(data->stmt_get_meta_id_by_message_id) mysql_stmt_close(data->stmt_get_meta_id_by_message_id);
   if(data->stmt_insert_into_rcpt_table) mysql_stmt_close(data->stmt_insert_into_rcpt_table);
   if(data->stmt_update_metadata_reference) mysql_stmt_close(data->stmt_update_metadata_reference);
}

