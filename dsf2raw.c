// dsf2raw.c - Convert a DSF file to raw DSD_U32_BE for testing with aplay.
//
//   gcc -O2 -o dsf2raw dsf2raw.c
//   ./dsf2raw input.dsf output.raw
//   aplay -D hw:3,0 -f DSD_U32_BE -c 2 -r 176400 output.raw   # DSD128
//   aplay -D hw:3,0 -f DSD_U32_BE -c 2 -r 88200  output.raw   # DSD64
//
// This uses the exact same packing as aplay+'s native DSD path so we can verify
// the packing independently of aplay+'s ALSA streaming. It also writes THREE
// variants so we can quickly find which one the DAC accepts:
//   output.raw       - bit-reversed, big-endian byte order  (current aplay+ logic)
//   output.norev.raw - no bit reversal, big-endian byte order
//   output.lerev.raw - bit-reversed, little-endian byte order

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint32_t read_le32(const uint8_t* b){return (uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24);}
static uint64_t read_le64(const uint8_t* b){uint64_t r=0;for(int i=0;i<8;i++)r|=((uint64_t)b[i])<<(8*i);return r;}

static inline uint8_t bitrev8(uint8_t v){
    return (uint8_t)((v * 0x0202020202ULL & 0x010884422010ULL) % 1023);
}

int main(int argc, char** argv){
    if(argc<3){fprintf(stderr,"usage: %s in.dsf out_prefix\n",argv[0]);return 1;}
    FILE* f=fopen(argv[1],"rb");
    if(!f){perror("open");return 1;}

    uint8_t hdr[80];
    if(fread(hdr,1,28,f)!=28||memcmp(hdr,"DSD ",4)){fprintf(stderr,"not DSD\n");return 1;}
    fseek(f,28,SEEK_SET);
    if(fread(hdr,1,52,f)!=52||memcmp(hdr,"fmt ",4)){fprintf(stderr,"no fmt\n");return 1;}
    uint64_t fmt_size=read_le64(hdr+4);
    int channels=read_le32(hdr+24);
    int dsd_rate=read_le32(hdr+28);
    uint64_t total_samples=read_le64(hdr+36);
    uint32_t block_bytes=read_le32(hdr+44);

    fprintf(stderr,"channels=%d dsd_rate=%d block_bytes=%u total_samples=%lu native_rate=%d\n",
            channels,dsd_rate,block_bytes,(unsigned long)total_samples,dsd_rate/32);

    fseek(f,28+fmt_size,SEEK_SET);
    uint8_t dchunk[12];
    if(fread(dchunk,1,12,f)!=12||memcmp(dchunk,"data",4)){fprintf(stderr,"no data chunk\n");return 1;}
    // file pointer now at start of audio data

    long data_start=ftell(f);
    size_t block_buf_size=(size_t)block_bytes*channels;
    uint8_t* block=malloc(block_buf_size);

    char name[3][512];
    snprintf(name[0],512,"%s.raw",argv[2]);        // bitrev + BE
    snprintf(name[1],512,"%s.norev.raw",argv[2]);  // no rev + BE
    snprintf(name[2],512,"%s.lerev.raw",argv[2]);  // bitrev + LE
    FILE* out[3];
    for(int i=0;i<3;i++){out[i]=fopen(name[i],"wb");}

    uint64_t total_native = (total_samples/8/* bytes per ch */)/4; // 4 bytes -> 1 frame
    (void)total_native;

    size_t block_size_bits=(size_t)block_bytes*8;

    // Read all blocks
    for(;;){
        size_t got=fread(block,1,block_buf_size,f);
        if(got==0) break;

        // Walk this block 4 bytes (32 bits) at a time per channel
        for(size_t bit=0; bit+32<=block_size_bits; bit+=32){
            size_t byte_idx=bit/8;
            for(int ch=0; ch<channels; ch++){
                const uint8_t* cd=block+(ch*block_bytes);
                uint8_t b0=cd[byte_idx+0],b1=cd[byte_idx+1],b2=cd[byte_idx+2],b3=cd[byte_idx+3];

                // variant 0: bitrev + BE
                uint8_t v0[4]={bitrev8(b0),bitrev8(b1),bitrev8(b2),bitrev8(b3)};
                fwrite(v0,1,4,out[0]);

                // variant 1: no rev + BE
                uint8_t v1[4]={b0,b1,b2,b3};
                fwrite(v1,1,4,out[1]);

                // variant 2: bitrev + LE (reversed byte order)
                uint8_t v2[4]={bitrev8(b3),bitrev8(b2),bitrev8(b1),bitrev8(b0)};
                fwrite(v2,1,4,out[2]);
            }
        }
    }

    for(int i=0;i<3;i++) fclose(out[i]);
    free(block);
    fclose(f);
    (void)data_start;
    fprintf(stderr,"wrote:\n  %s (bitrev+BE)\n  %s (norev+BE)\n  %s (bitrev+LE)\n",name[0],name[1],name[2]);
    return 0;
}
