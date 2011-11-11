/*
 * Copyright (c) 2011 Moriyoshi Koizumi
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <curl/curl.h>
#include <libxml/parser.h>

#include "airbrake.h"

struct airbrake_client_opaque_t {
    CURL *curl;
};

airbrake_client_info_t airbrake_default_client_info = {
    "libairbrake",
    AIRBRAKE_VERSION_STRING,
    "http://github.com/moriyoshi/libairbrake"
};

airbrake_string_t airbrake_default_notice_endpoint_url = AIRBRAKE_STRING_STATIC("http://airbrake.io/notifier_api/v2/notices");

typedef struct airbrake_curl_writer_t {
    airbrake_string_t *buf;
} airbrake_curl_writer_t;

static size_t airbrake_curl_writer_func(char *ptr, size_t size, size_t nmemb, airbrake_curl_writer_t *writer)
{
    size_t nbytes = size * nmemb;
    if (airbrake_string_append(writer->buf, airbrake_string_static(ptr, nbytes))) {
        return CURLE_WRITE_ERROR;
    }
    return nbytes;
}

airbrake_error_t airbrake_string_init(airbrake_string_t *string, const char *str, size_t str_len)
{
    char *p;
    if (!str) {
        string->p = 0;
        string->l = 0;
        string->al = 0;
        return AIRBRAKE_OK;
    }
    p = malloc(str_len + 1);
    if (!p)
        return AIRBRAKE_ERROR_MEM;
    memmove(p, str, str_len);
    p[str_len] = 0;
    string->p = p;
    string->l = str_len;
    string->al = str_len;
    return AIRBRAKE_OK;
}

airbrake_error_t airbrake_string_init_c(airbrake_string_t *string, const airbrake_string_t *orig)
{
    return airbrake_string_init(string, orig->p, orig->l);
}

inline airbrake_error_t airbrake_string_grow(airbrake_string_t *string, size_t new_cap)
{
    size_t requested_al = new_cap + 1, new_al = 0;
    char *new_p;

    if (requested_al == 0)
        return AIRBRAKE_ERROR_MEM;
    if (string->al >= requested_al)
        return AIRBRAKE_OK;
    new_al = 1;
    while (new_al < requested_al) {
        new_al <<= 1;
        if (new_al == 0)
            return AIRBRAKE_ERROR_MEM;
    }
    new_p = realloc(string->p, new_al);
    if (!new_p)
        return AIRBRAKE_ERROR_MEM;
    string->p = new_p;
    string->al = new_al;
    return AIRBRAKE_OK;
}

airbrake_error_t airbrake_string_append(airbrake_string_t *string, airbrake_string_t other)
{
    airbrake_error_t err = airbrake_string_grow(string, string->l + other.l);
    if (err)
        return err;
    memmove(string->p + string->l, other.p, other.l);
    string->l += other.l;
    string->p[string->l] = 0;
    return AIRBRAKE_OK;
}

void airbrake_string_fini(airbrake_string_t *string)
{
    if (string->al > 0) {
        if (string->p)
            free(string->p);
    }
    string->p = 0;
}

static airbrake_error_t airbrake_string_table_entry_init(airbrake_string_table_entry_t *entry, const airbrake_string_t *key, const airbrake_string_t *value)
{
    airbrake_error_t err;
    entry->next = entry->prev = 0;
    err = airbrake_string_init_c(&entry->key, key);
    if (err)
        return err;
    err = airbrake_string_init_c(&entry->value, value);
    if (err) {
        airbrake_string_fini(&entry->key);
        return err;
    }
    return AIRBRAKE_OK;
}

static void airbrake_string_table_entry_fini(airbrake_string_table_entry_t *entry)
{
    airbrake_string_fini(&entry->key);
    airbrake_string_fini(&entry->value);
}

airbrake_error_t airbrake_string_table_init(airbrake_string_table_t *table)
{
    table->first = table->last = 0;
    return AIRBRAKE_OK;
}

void airbrake_string_table_fini(airbrake_string_table_t *table)
{
    airbrake_string_table_entry_t *i, *next;
    for (i = table->first; i; i = next) {
        next = i->next;
        airbrake_string_table_entry_fini(i);
        free(i);
    }
    table->first = table->last = 0;
}

airbrake_error_t airbrake_string_table_add(airbrake_string_table_t *table, airbrake_string_t key, airbrake_string_t value)
{
    airbrake_string_table_entry_t *new_entry = malloc(sizeof(airbrake_string_table_entry_t));
    if (!new_entry)
        return AIRBRAKE_ERROR_MEM;
    airbrake_string_table_entry_init(new_entry, &key, &value);

    if (table->last) {
        table->last->next = new_entry;
    } else {
        table->first = new_entry;
    }
    new_entry->prev = table->last;
    table->last = new_entry;
    return AIRBRAKE_OK;
}

airbrake_error_t airbrake_request_info_init(airbrake_request_info_t *request_info, airbrake_string_t url, airbrake_string_t component, airbrake_string_t action)
{
    airbrake_error_t err = AIRBRAKE_OK;
    request_info->url.p = 0;
    request_info->component.p = 0;
    request_info->action.p = 0;
    request_info->params.first = 0;
    request_info->session.first = 0;
    request_info->cgi_data.first = 0;
    err = airbrake_string_init_c(&request_info->url, &url);
    if (err)
        goto fail;
    err = airbrake_string_init_c(&request_info->component, &component);
    if (err)
        goto fail;
    err = airbrake_string_init_c(&request_info->action, &action);
    if (err)
        goto fail;
    err = airbrake_string_table_init(&request_info->params);
    if (err)
        goto fail;
    err = airbrake_string_table_init(&request_info->session);
    if (err)
        goto fail;
    err = airbrake_string_table_init(&request_info->cgi_data);
    if (err)
        goto fail;
    return AIRBRAKE_OK;
fail:
    airbrake_string_fini(&request_info->url);
    airbrake_string_fini(&request_info->component);
    airbrake_string_fini(&request_info->action);
    airbrake_string_table_init(&request_info->params);
    airbrake_string_table_init(&request_info->session);
    airbrake_string_table_init(&request_info->cgi_data);
    return err;
}

void airbrake_request_info_fini(airbrake_request_info_t *request_info)
{
    airbrake_string_fini(&request_info->url);
    airbrake_string_fini(&request_info->component);
    airbrake_string_fini(&request_info->action);
    airbrake_string_table_fini(&request_info->params);
    airbrake_string_table_fini(&request_info->session);
    airbrake_string_table_fini(&request_info->cgi_data);
}

airbrake_error_t airbrake_environment_info_init(airbrake_environment_info_t *environment_info, airbrake_string_t project_root, airbrake_string_t environment_name, airbrake_string_t app_version)
{
    airbrake_error_t err = AIRBRAKE_OK;
    environment_info->project_root.p = 0;
    environment_info->environment_name.p = 0;
    environment_info->app_version.p = 0;

    err = airbrake_string_init_c(&environment_info->project_root, &project_root);
    if (err)
        goto fail;

    err = airbrake_string_init_c(&environment_info->environment_name, &environment_name);
    if (err)
        goto fail;

    err = airbrake_string_init_c(&environment_info->app_version, &app_version);
    if (err)
        goto fail;

    return AIRBRAKE_OK;
fail:
    airbrake_string_fini(&environment_info->project_root);
    airbrake_string_fini(&environment_info->environment_name);
    airbrake_string_fini(&environment_info->app_version);
    return err; 
}

void airbrake_environment_info_fini(airbrake_environment_info_t *environment_info)
{
    airbrake_string_fini(&environment_info->project_root);
    airbrake_string_fini(&environment_info->environment_name);
    airbrake_string_fini(&environment_info->app_version);
}

static airbrake_error_t airbrake_backtrace_entry_init(airbrake_backtrace_entry_t *entry, const airbrake_string_t *method, const airbrake_string_t *file, int line)
{
    airbrake_error_t err = AIRBRAKE_OK;
    entry->next = entry->prev = 0;
    entry->method.p = 0;
    entry->file.p = 0;
    entry->line = line;
    err = airbrake_string_init_c(&entry->method, method);
    if (err)
        goto fail;
    err = airbrake_string_init_c(&entry->file, file);
    if (err)
        goto fail;
    return AIRBRAKE_OK;
fail:
    airbrake_string_fini(&entry->method);
    airbrake_string_fini(&entry->file);
}

static void airbrake_backtrace_entry_fini(airbrake_backtrace_entry_t *entry)
{
    airbrake_string_fini(&entry->method);
    airbrake_string_fini(&entry->file);
}

airbrake_error_t airbrake_backtrace_init(airbrake_backtrace_t *backtrace)
{
    backtrace->first = backtrace->last = 0;
    return AIRBRAKE_OK;
}

void airbrake_backtrace_fini(airbrake_backtrace_t *backtrace)
{
    airbrake_backtrace_entry_t *i, *next;
    for (i = backtrace->first; i; i = next) {
        next = i->next;
        airbrake_backtrace_entry_fini(i);
        free(i);
    }
    backtrace->first = backtrace->last = 0;
}

airbrake_error_t airbrake_backtrace_add_entry(airbrake_backtrace_t *backtrace, airbrake_string_t method, airbrake_string_t file, int line)
{
    airbrake_backtrace_entry_t *new_entry = malloc(sizeof(airbrake_backtrace_entry_t));
    if (!new_entry)
        return AIRBRAKE_ERROR_MEM;
    airbrake_backtrace_entry_init(new_entry, &method, &file, line);

    if (backtrace->last) {
        backtrace->last->next = new_entry;
    } else {
        backtrace->first = new_entry;
    }
    new_entry->prev = backtrace->last;
    backtrace->last = new_entry;
    return AIRBRAKE_OK;
}

airbrake_error_t airbrake_exception_init(airbrake_exception_t *exception, airbrake_string_t klass, airbrake_string_t message)
{
    airbrake_error_t err = AIRBRAKE_OK;
    exception->backtrace = 0;
    exception->klass.p = 0;
    exception->message.p = 0;

    err = airbrake_string_init_c(&exception->klass, &klass);
    if (err)
        goto fail;
    err = airbrake_string_init_c(&exception->message, &message);
    if (err)
        goto fail;

    return AIRBRAKE_OK;
fail:
    airbrake_string_fini(&exception->klass);
    airbrake_string_fini(&exception->message);
    return err;
}

void airbrake_exception_fini(airbrake_exception_t *exception)
{
    airbrake_string_fini(&exception->klass);
    airbrake_string_fini(&exception->message);
    if (exception->backtrace) {
        airbrake_backtrace_fini(exception->backtrace);
        free(exception->backtrace);
        exception->backtrace = 0;
    }
}

airbrake_error_t airbrake_notice_init(airbrake_notice_t *notice)
{
    notice->exception = 0;
    notice->request = 0;
    notice->environment = 0;
    return AIRBRAKE_OK;
}

void airbrake_notice_fini(airbrake_notice_t *notice)
{
}

airbrake_error_t airbrake_notice_result_init(airbrake_notice_result_t *notice_result, airbrake_string_t error_id, airbrake_string_t url, airbrake_string_t id)
{
    airbrake_error_t err;

    err = airbrake_string_init_c(&notice_result->error_id, &error_id);
    if (err)
        return err;
    err = airbrake_string_init_c(&notice_result->url, &url);
    if (err)
        return err;
    err = airbrake_string_init_c(&notice_result->id, &id);
    if (err)
        return err;

    return AIRBRAKE_OK;
fail:
    airbrake_string_fini(&notice_result->error_id);
    airbrake_string_fini(&notice_result->url);
    airbrake_string_fini(&notice_result->id);
    return err;
}

void airbrake_notice_result_fini(airbrake_notice_result_t *notice_result)
{
    airbrake_string_fini(&notice_result->error_id);
    airbrake_string_fini(&notice_result->url);
    airbrake_string_fini(&notice_result->id);
}


airbrake_error_t airbrake_client_opaque_init(airbrake_client_opaque_t **data)
{
    airbrake_client_opaque_t *_data = malloc(sizeof(airbrake_client_opaque_t));
    if (!_data)
        return AIRBRAKE_ERROR_MEM;
    _data->curl = curl_easy_init();
    if (!_data) {
        free(_data);
        return AIRBRAKE_ERROR_UNKNOWN;
    }
    *data = _data;
    return AIRBRAKE_OK;
}

void airbrake_client_opaque_fini(airbrake_client_opaque_t **data)
{
    curl_easy_cleanup((*data)->curl);
    free(*data);
    *data = 0;
}

airbrake_error_t airbrake_client_init(airbrake_client_t *client, const airbrake_client_info_t *info, airbrake_string_t notice_endpoint, airbrake_string_t api_key)
{
    airbrake_error_t err;
    client->info = info ? info: &airbrake_default_client_info;
    err = airbrake_client_opaque_init(&client->priv);
    if (err)
        return err;
    err = airbrake_string_init_c(&client->notice_endpoint, &notice_endpoint);
    if (err) {
        airbrake_client_opaque_fini(&client->priv);
        return err;
    }
    err = airbrake_string_init_c(&client->api_key, &api_key);
    if (err) {
        airbrake_string_fini(&client->notice_endpoint);
        airbrake_client_opaque_fini(&client->priv);
        return err;
    }
    return AIRBRAKE_OK;
}

static airbrake_error_t airbrake_string_append_xml_escape(airbrake_string_t *string, const airbrake_string_t *src)
{
    airbrake_error_t err;
    size_t o = string->l;
    const char *p = src->p, *e = src->p + src->l;

    err = airbrake_string_grow(string, string->l + src->l);
    if (err)
        return err;

    for (; p < e; p++) {
        if (*p == '<') {
            err = airbrake_string_grow(string, o + 4);
            if (err)
                return err;
            memmove(&string->p[o], "&lt;", 4);
            o += 4;
        } else if (*p == '>') {
            err = airbrake_string_grow(string, o + 4);
            if (err)
                return err;
            memmove(&string->p[o], "&gt;", 4);
            o += 4;
        } else if (*p == '&') {
            err = airbrake_string_grow(string, o + 5);
            if (err)
                return err;
            memmove(&string->p[o], "&amp;", 5);
            o += 5;
        } else if (*p == '"') {
            err = airbrake_string_grow(string, o + 6);
            if (err)
                return err;
            memmove(&string->p[o], "&quot;", 6);
            o += 6;
        } else {
            err = airbrake_string_grow(string, o + 1);
            if (err)
                return err;
            string->p[o] = *p;
            o++;
        }
    }
    string->p[o] = 0;
    string->l = o;
    return AIRBRAKE_OK;
}

static airbrake_error_t airbrake_client_build_notice_xml_notifier(const airbrake_client_info_t *info, airbrake_string_t *buf)
{
    airbrake_error_t err;

    err = airbrake_string_append(buf, airbrake_string_static_z(
          "<notifier>"
            "<name>"));
    if (err)
        return err;
    {
        airbrake_string_t tmp = airbrake_string_static_z(info->name);
        err = airbrake_string_append_xml_escape(buf, &tmp);
        if (err)
            return err;
    }
    err = airbrake_string_append(buf, airbrake_string_static_z(
            "</name>"
            "<version>"));
    if (err)
        return err;
    {
        airbrake_string_t tmp = airbrake_string_static_z(info->version);
        err = airbrake_string_append_xml_escape(buf, &tmp);
        if (err)
            return err;
    }
    err = airbrake_string_append(buf, airbrake_string_static_z(
            "</version>"
            "<url>"));
    if (err)
        return err;
    {
        airbrake_string_t tmp = airbrake_string_static_z(info->url);
        err = airbrake_string_append_xml_escape(buf, &tmp);
        if (err)
            return err;
    }
    err = airbrake_string_append(buf, airbrake_string_static_z(
            "</url>"
          "</notifier>"));
    return err;
}

static airbrake_error_t airbrake_client_build_notice_xml_backtrace(const airbrake_backtrace_t *backtrace, airbrake_string_t *buf)
{
    airbrake_error_t err;
    airbrake_backtrace_entry_t *i;

    err = airbrake_string_append(buf, airbrake_string_static_z(
          "<backtrace>"));
    if (err)
        return err;

    for (i = backtrace->first; i; i = i->next) {
        err = airbrake_string_append(buf, airbrake_string_static_z(
              "<line method=\""));
        if (err)
            return err;
        err = airbrake_string_append_xml_escape(buf, &i->method);
        if (err)
            return err;
        err = airbrake_string_append(buf, airbrake_string_static_z(
              "\" file=\""));
        if (err)
            return err;
        err = airbrake_string_append_xml_escape(buf, &i->file);
        if (err)
            return err;
        err = airbrake_string_append(buf, airbrake_string_static_z(
              "\" number=\""));
        if (err)
            return err;
        {
            char tmp[128];
            snprintf(tmp, sizeof(tmp), "%d", i->line);
            err = airbrake_string_append(buf, airbrake_string_static_z(tmp));
            if (err)
                return err;
        }
        err = airbrake_string_append(buf, airbrake_string_static_z(
              "\" />"));
        if (err)
            return err;
    }

    err = airbrake_string_append(buf, airbrake_string_static_z(
          "</backtrace>"));
    return err;
}

static airbrake_error_t airbrake_client_build_notice_xml_error(const airbrake_exception_t *exception, airbrake_string_t *buf)
{
    airbrake_error_t err;

    err = airbrake_string_append(buf, airbrake_string_static_z(
          "<error>"
            "<class>"));
    if (err)
        return err;
    err = airbrake_string_append_xml_escape(buf, &exception->klass);
    if (err)
        return err;
    err = airbrake_string_append(buf, airbrake_string_static_z(
            "</class>"
            "<message>"));
    err = airbrake_string_append_xml_escape(buf, &exception->message);
    if (err)
        return err;
    err = airbrake_string_append(buf, airbrake_string_static_z(
            "</message>"));
    if (exception->backtrace) {
        err = airbrake_client_build_notice_xml_backtrace(exception->backtrace, buf);
        if (err)
            return err;
    }
    err = airbrake_string_append(buf, airbrake_string_static_z(
          "</error>"));
    return err;
}

static airbrake_error_t airbrake_client_build_notice_xml_params(const airbrake_string_table_t *table, const char *tagname, airbrake_string_t *buf)
{
    airbrake_error_t err;

    if (!table->first)
        return AIRBRAKE_OK;

    err = airbrake_string_append(buf, airbrake_string_static_z(
            "<"));
    if (err)
        return err;
    err = airbrake_string_append(buf, airbrake_string_static_z(tagname));
    if (err)
        return err;
    err = airbrake_string_append(buf, airbrake_string_static_z(
            ">"));
    if (err)
        return err;
    {
        airbrake_string_table_entry_t *i;
        for (i = table->first; i; i = i->next) {
            err = airbrake_string_append(buf, airbrake_string_static_z("<var key=\""));
            if (err)
                return err;
            err = airbrake_string_append_xml_escape(buf, &i->key);
            if (err)
                return err;
            err = airbrake_string_append(buf, airbrake_string_static_z("\">"));
            if (err)
                return err;
            err = airbrake_string_append_xml_escape(buf, &i->value);
            if (err)
                return err;
            err = airbrake_string_append(buf, airbrake_string_static_z("</var>"));
            if (err)
                return err;
        }
    }
    err = airbrake_string_append(buf, airbrake_string_static_z(
            "</"));
    if (err)
        return err;
    err = airbrake_string_append(buf, airbrake_string_static_z(tagname));
    if (err)
        return err;
    err = airbrake_string_append(buf, airbrake_string_static_z(
            ">"));
    return err;
}

static airbrake_error_t airbrake_client_build_notice_xml_request(const airbrake_request_info_t *request, airbrake_string_t *buf)
{
    airbrake_error_t err;

    err = airbrake_string_append(buf, airbrake_string_static_z(
          "<request>"
            "<url>"));
    if (err)
        return err;
    err = airbrake_string_append_xml_escape(buf, &request->url);
    if (err)
        return err;
    err = airbrake_string_append(buf, airbrake_string_static_z(
            "</url>"
            "<component>"));
    if (err)
        return err;
    err = airbrake_string_append_xml_escape(buf, &request->component);
    if (err)
        return err;
    err = airbrake_string_append(buf, airbrake_string_static_z(
            "</component>"
            "<action>"));
    if (err)
        return err;
    err = airbrake_string_append_xml_escape(buf, &request->action);
    if (err)
        return err;
    err = airbrake_string_append(buf, airbrake_string_static_z(
            "</action>"));
    if (err)
        return err;

    err = airbrake_client_build_notice_xml_params(&request->params, "params", buf);
    if (err)
        return err;

    err = airbrake_client_build_notice_xml_params(&request->session, "session", buf);
    if (err)
        return err;

    err = airbrake_client_build_notice_xml_params(&request->cgi_data, "cgi-data", buf);
    if (err)
        return err;

    err = airbrake_string_append(buf, airbrake_string_static_z(
          "</request>"));
    return err;
}

static airbrake_error_t airbrake_client_build_notice_xml_server_environment(const airbrake_environment_info_t *environment, airbrake_string_t *buf)
{
    airbrake_error_t err;

    err = airbrake_string_append(buf, airbrake_string_static_z(
          "<server-environment>"));
    if (environment->project_root.p) {
        err = airbrake_string_append(buf, airbrake_string_static_z(
                "<project-root>"));
        if (err)
            return err;
        err = airbrake_string_append_xml_escape(buf, &environment->project_root);
        if (err)
            return err;
        err = airbrake_string_append(buf, airbrake_string_static_z(
                "</project-root>"));
    }
    err = airbrake_string_append(buf, airbrake_string_static_z(
        "<environment-name>"));
    if (err)
        return err;
    err = airbrake_string_append_xml_escape(buf, &environment->environment_name);
    if (err)
        return err;
    err = airbrake_string_append(buf, airbrake_string_static_z(
            "</environment-name>"));

    if (environment->app_version.p) {
        err = airbrake_string_append(buf, airbrake_string_static_z(
                "<app-version>"));
        if (err)
            return err;
        err = airbrake_string_append_xml_escape(buf, &environment->app_version);
        if (err)
            return err;
        err = airbrake_string_append(buf, airbrake_string_static_z(
                "</app-version>"));
    }
    err = airbrake_string_append(buf, airbrake_string_static_z(
          "</server-environment>"));
    return err;
}

airbrake_error_t airbrake_client_build_notice_xml(airbrake_client_t *client, airbrake_string_t *buf, const airbrake_notice_t *notice)
{
    airbrake_error_t err;
    err = airbrake_string_append(buf, airbrake_string_static_z(
        "<?xml version=\"1.0\" ?>"
        "<notice version=\"2.0\">"
          "<api-key>"));
    if (err)
        return err;
    err = airbrake_string_append_xml_escape(buf, &client->api_key);
    if (err)
        return err;
    err = airbrake_string_append(buf, airbrake_string_static_z(
          "</api-key>"));
    if (err)
        return err;
    err = airbrake_client_build_notice_xml_notifier(client->info, buf);
    if (err)
        return err;

    err = airbrake_client_build_notice_xml_error(notice->exception, buf);
    if (err)
        return err;

    err = airbrake_client_build_notice_xml_request(notice->request, buf);
    if (err)
        return err;

    err = airbrake_client_build_notice_xml_server_environment(notice->environment, buf);
    if (err)
        return err;

    err = airbrake_string_append(buf, airbrake_string_static_z(
        "</notice>"));
    return err;
}

airbrake_error_t airbrake_client_submit_notice(airbrake_client_t *client, airbrake_notice_result_t *result, const airbrake_notice_t *notice)
{
    airbrake_error_t err = AIRBRAKE_OK;
    airbrake_string_t buf = { 0, 0, 0 };

    result->error_id.p = 0;
    result->url.p = 0;
    result->id.p = 0;

    err = airbrake_client_build_notice_xml(client, &buf, notice);
    if (err) {
        airbrake_string_fini(&buf);
        return err;
    }

    {
        CURL *curl = client->priv->curl;
        airbrake_string_t out_buf = { 0, 0, 0 };
        airbrake_curl_writer_t writer = { &out_buf };
        xmlParserCtxtPtr parser = 0;
        xmlDocPtr doc = 0;
        xmlNodePtr root_node = 0;
        const char *content_type_header_value;
        airbrake_string_t content_type = { 0, 0, 0 };
        airbrake_string_t charset = { 0, 0, 0 };

        curl_easy_setopt(curl, CURLOPT_URL, client->notice_endpoint.p);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, buf.p);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, buf.l);
        curl_easy_setopt(curl, CURLOPT_POST, 1);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, airbrake_curl_writer_func);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &writer);
        if (curl_easy_perform(curl)) {
            err = AIRBRAKE_ERROR_NETWORK_FAILURE;
            goto out_inner;
        }
        curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type_header_value);
        if (content_type_header_value) {
            do {
                const char *p = content_type_header_value, *q;
                size_t l;

                while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
                if (!*p)
                    break;
                content_type.p = (char *)p;

                while (*p && *p != ';' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
                content_type.l = p - content_type.p;

                while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
                if (*p != ';')
                    break;
                p++;
                while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
                if (!*p)
                    break;

                l = strlen(p);
                if (l <= 8 || memcmp(p, "charset", 7) != 0)
                    break;
                p += 7;
                while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
                if (*p != '=')
                    break;
                p++;
                while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
                if (*p)
                    break;
                q = p;
                while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
                l = p - charset.p;
                err = airbrake_string_init(&charset, q, l);
                if (err)
                    goto out_inner;
            } while (0);
        }

        if (!(content_type.l == 8 && memcmp("text/xml", content_type.p, 8) == 0) &&
            !(content_type.l == 9 && memcmp("application/xml", content_type.p, 15) == 0)) {
            err = AIRBRAKE_ERROR_INVALID_RESPONSE;
            goto out_inner;
        }

        parser = xmlNewParserCtxt();
        if (!parser) {
            err = AIRBRAKE_ERROR_MEM;
            goto out_inner;
        }

        doc = xmlCtxtReadDoc(parser, out_buf.p, 0, charset.p, 0);
        if (!doc) {
            err = AIRBRAKE_ERROR_INVALID_RESPONSE;
            goto out_inner;
        }

        root_node = xmlDocGetRootElement(doc);
        if (strcmp(root_node->name, "notice") != 0) {
            err = AIRBRAKE_ERROR_INVALID_RESPONSE;
            goto out_inner;
        }
        {
            xmlNodePtr node;
            for (node = root_node->children; node; node = node->next) {
                if (node->type != XML_ELEMENT_NODE)
                    continue;
                if (strcmp(node->name, "error-id") == 0) {
                    if (!node->children || node->children->type != XML_TEXT_NODE) {
                        err = AIRBRAKE_ERROR_INVALID_RESPONSE;
                        goto out_inner;
                    }
                    airbrake_string_init(&result->error_id, node->children->content, strlen(node->children->content));
                } else if (strcmp(node->name, "url") == 0) {
                    if (!node->children || node->children->type != XML_TEXT_NODE) {
                        err = AIRBRAKE_ERROR_INVALID_RESPONSE;
                        goto out_inner;
                    }
                    airbrake_string_init(&result->url, node->children->content, strlen(node->children->content));
                } else if (strcmp(node->name, "id") == 0) {
                    if (!node->children || node->children->type != XML_TEXT_NODE) {
                        err = AIRBRAKE_ERROR_INVALID_RESPONSE;
                        goto out_inner;
                    }
                    airbrake_string_init(&result->id, node->children->content, strlen(node->children->content));
                }
            }
        }

    out_inner:
        airbrake_string_fini(&out_buf);
        airbrake_string_fini(&content_type);
        airbrake_string_fini(&charset);
        if (doc)
            xmlFreeDoc(doc);
        if (parser)
            xmlFreeParserCtxt(parser);
    }
    if (err)
        airbrake_notice_result_fini(result);
    airbrake_string_fini(&buf);
    return err;
}

void airbrake_client_fini(airbrake_client_t *client)
{
    airbrake_client_opaque_fini(&client->priv);
    airbrake_string_fini(&client->notice_endpoint);
    airbrake_string_fini(&client->api_key);
}

void airbrake_init()
{
    curl_global_init(CURL_GLOBAL_ALL);
    xmlInitParser();
}

void airbrake_cleanup()
{
    xmlCleanupParser();
    curl_global_cleanup();
}

