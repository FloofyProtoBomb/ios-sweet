/* SPDX-License-Identifier: MIT -- checks the real environment profile only. */
#define _POSIX_C_SOURCE 200809L
#include "base_environment.h"
#include <stdio.h>
static int check(int wanted)
{return dre_metal306_base_environment_allowed()==wanted;}
int main(void)
{
    const char *a="MTL_DUMP_PIPELINES_TO_JSON_FILE",*b="MTL_DUMP_LIBRARIES_TO_DIRECTORY";
    if(unsetenv(a)||unsetenv(b)||!check(1)||setenv(a,"",1)||!check(0)||
       setenv(a,"/tmp/unused-profile-path",1)||!check(0)||unsetenv(a)||!check(1)||
       setenv(b,"",1)||!check(0)||setenv(a,"",1)||!check(0)||
       unsetenv(a)||unsetenv(b)||!check(1))return 1;
    puts("PASS: selected base dump branches refuse absent/present/empty environment cases; no device constructed");
    return 0;
}
