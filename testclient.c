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
#include <stdio.h>
#include "airbrake.h"

airbrake_error_t doit(const char *api_key)
{
    airbrake_error_t err = AIRBRAKE_OK;
    airbrake_client_t client;
    int client_initialized = 0;
    err = airbrake_client_init(&client, 0, airbrake_default_notice_endpoint_url, airbrake_string_static_z(api_key));
    if (err)
        goto out;
    client_initialized = 1;

    {
        airbrake_backtrace_t *backtrace;
        airbrake_exception_t exception;
        airbrake_request_info_t request;
        airbrake_environment_info_t environment;

        airbrake_notice_t notice;

        backtrace = malloc(sizeof(airbrake_backtrace_t));
        if (!backtrace) {
            err = AIRBRAKE_ERROR_MEM;
            goto out;
        }

        err = airbrake_backtrace_init(backtrace);
        if (err)
            goto out;
        err = airbrake_backtrace_add_entry(backtrace, airbrake_string_static_z("method1"), airbrake_string_static_z("file.py"), 1);
        if (err) {
            airbrake_backtrace_fini(backtrace);
            goto out;
        }
        err = airbrake_backtrace_add_entry(backtrace, airbrake_string_static_z("method2"), airbrake_string_static_z("file.py"), 2);
        if (err) {
            airbrake_backtrace_fini(backtrace);
            goto out;
        }
        err = airbrake_exception_init(&exception, airbrake_string_static_z("SomeClass"), airbrake_string_static_z("some message"));
        if (err) {
            airbrake_backtrace_fini(backtrace);
            goto out;
        }
        exception.backtrace = backtrace;

        err = airbrake_request_info_init(&request, airbrake_string_static_z("request_url"), airbrake_string_static_z("some_component"), airbrake_string_static_z("some_action"));
        if (err) {
            airbrake_exception_fini(&exception);
            goto out;
        }

        airbrake_string_table_add(&request.cgi_data, airbrake_string_static_z("env_1"), airbrake_string_static_z("env_1_value<>&\""));
        airbrake_string_table_add(&request.cgi_data, airbrake_string_static_z("env_2"), airbrake_string_static_z("env_2_value<>&\""));

        err = airbrake_environment_info_init(&environment, airbrake_string_static_z("project_root"), airbrake_string_static_z("environment_name"), airbrake_string_static_z("app_version"));
        if (err) {
            airbrake_backtrace_fini(backtrace);
            airbrake_exception_fini(&exception);
            airbrake_request_info_fini(&request);
            goto out;
        }

        err = airbrake_notice_init(&notice);
        if (err) {
            airbrake_backtrace_fini(backtrace);
            airbrake_exception_fini(&exception);
            airbrake_request_info_fini(&request);
            airbrake_environment_info_fini(&environment);
            goto out;
        }

        notice.exception = &exception;
        notice.request = &request;
        notice.environment = &environment;

        {
            airbrake_notice_result_t result;
            err = airbrake_client_submit_notice(&client, &result, &notice);
            if (!err) {
                printf("error_id: %s, url: %s, id: %s\n", result.error_id.p, result.url.p, result.id.p);
                airbrake_notice_result_fini(&result);
            }
        }

        airbrake_exception_fini(&exception);
        airbrake_request_info_fini(&request);
        airbrake_environment_info_fini(&environment);
        airbrake_notice_fini(&notice);
    }

out:
    if (client_initialized)
        airbrake_client_fini(&client);

    return err;
}

int main(int argc, char **argv)
{
    int status = 0;
    const char *api_key = getenv("AIRBRAKE_API_KEY");
    airbrake_init();

    if (!api_key) {
        fprintf(stderr, "please supply AIRBRAKE_API_KEY environment variable\n");
        return 1;
    }

    if (doit(api_key))
        status = 1;

    airbrake_cleanup();
    return status;
}
