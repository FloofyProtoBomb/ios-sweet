/* SPDX-License-Identifier: MIT */
#include "compiler_request.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    uint8_t output[128];size_t used=99,size=99;
    static const uint8_t golden[]={
        3,0,0,0, 0,0,0,0, 2,0,0,0, 0,0,0,0,
        'x','y',0,0, 'z',0};
    memset(output,0xa5,sizeof(output));
    assert(dre_metal306_request13("xy",2,"z",1,output,sizeof(output),&used));
    assert(used==sizeof(golden) && !memcmp(output,golden,sizeof(golden)) && output[used]==0xa5);
    for(size_t n=1;n<9;++n) {
        assert(dre_metal306_request13("123456789",n,"-x metal",8,output,sizeof(output),&used));
        size_t aligned=(16+n+1+3)&~(size_t)3;
        assert(output[0]==n+1 && output[4]==0 && output[8]==9 && output[12]==0);
        assert(output[16+n]==0 && !memcmp(output+aligned,"-x metal\0",9));
        assert(used==aligned+9);
    }
    assert(!dre_metal306_request13_size(0,1,&size) && !size);
    assert(!dre_metal306_request13_size(1,SIZE_MAX,&size) && !size);
    assert(!dre_metal306_request13_size(SIZE_MAX,1,&size) && !size);
    assert(!dre_metal306_request13("A\0B",3,"-x",2,output,sizeof(output),&used) && !used);
    assert(!dre_metal306_request13("AB",2,"-\0x",3,output,sizeof(output),&used) && !used);
    assert(!dre_metal306_request13("xy",2,"z",1,output,21,&used) && !used);
    memcpy(output,"xy",2);
    assert(!dre_metal306_request13(output,2,"z",1,output,sizeof(output),&used) && !used);
    assert(!dre_metal306_request13("xy",2,output,1,output,sizeof(output),&used) && !used);
    assert(!dre_metal306_request13(NULL,2,"z",1,output,sizeof(output),&used));
    assert(!dre_metal306_request13_size(1,1,NULL));
    struct dre_metal306_reply reply;
    uint8_t envelope[128]={0};envelope[0]=3;envelope[8]=104;envelope[12]=3;
    envelope[40]=107;envelope[44]=4;
    memcpy(envelope+104,"heyMTLB",7);
    assert(dre_metal306_reply(0,envelope,111,&reply));
    assert(reply.warnings==envelope+104 && reply.warning_bytes==3);
    assert(reply.metallib==envelope+107 && reply.metallib_bytes==4);
    assert(!dre_metal306_reply(1,envelope,111,&reply) && !reply.metallib);
    assert(!dre_metal306_reply(0,envelope,15,&reply) && !reply.metallib);
    envelope[8]=16;assert(!dre_metal306_reply(0,envelope,111,&reply));
    envelope[8]=104;envelope[12]=255;assert(!dre_metal306_reply(0,envelope,111,&reply));
    envelope[12]=3;envelope[107]='X';assert(!dre_metal306_reply(0,envelope,111,&reply));
    envelope[107]='M';envelope[40]=108;assert(!dre_metal306_reply(0,envelope,111,&reply));
    envelope[40]=107;envelope[44]=5;assert(!dre_metal306_reply(0,envelope,111,&reply));
    envelope[44]=4;envelope[48]=111;envelope[52]=2;
    assert(dre_metal306_reply(0,envelope,113,&reply) && reply.metallib_bytes==4);
    assert(!dre_metal306_reply(0,envelope,112,&reply));
    envelope[48]=110;assert(!dre_metal306_reply(0,envelope,113,&reply));
    assert(!dre_metal306_reply(0,NULL,111,&reply));
    assert(!dre_metal306_reply(0,envelope,111,NULL));
    puts("PASS: Metal306 bounded request/reply golden layout, alignment, alias, overflow, truncation and error controls; no compiler invoked");
    return 0;
}
