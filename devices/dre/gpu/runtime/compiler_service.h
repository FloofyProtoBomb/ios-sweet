/* SPDX-License-Identifier: MIT; UNVALIDATED source-only runtime draft. */
#pragma once
#import <Foundation/Foundation.h>
#include <stddef.h>
#include <stdint.h>

/* Real local compiler306 service, exact selected20H392 image only. Accepted
 * input is the qualified request13 subset, not a shader-success stub. Output
 * is an owned copied compiler archive span, never GPU execution or pipeline
 * acceptance. Request14 and every other unqualified packet return an error.
 * No plugin/service publication or compiler fallback is performed.
 */
NSData *dre_metal306_compile_request(uint32_t type,const void *request,size_t bytes,
    NSString **warnings,NSError **error);
