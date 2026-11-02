/* Exercise production file-copy I/O with controlled short/error returns. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int scenario, reads, writes, source_fd, target_fd;
static ssize_t ReadForTest(int fd, void *data, size_t length)
{
    source_fd = fd;
    if (scenario==1 && reads++==0) { errno=EINTR; return -1; }
    if (scenario==5 && reads++>0) { errno=EIO; return -1; }
    return read(fd,data,length);
}
static ssize_t WriteForTest(int fd, const void *data, size_t length)
{
    target_fd = fd;
    if (scenario==2 && writes++==0) { errno=EINTR; return -1; }
    if (scenario==3) return 0;
    if (scenario==4 && writes++>0) { errno=ENOSPC; return -1; }
    if (scenario==1 || scenario==2) {
        if (length>3) length=3;
    }
    return write(fd,data,length);
}
static int CloseForTest(int fd)
{
    int result=close(fd);
    if ((scenario==6 && fd==target_fd) || (scenario==7 && fd==source_fd)) {
        errno=EIO;
        return -1;
    }
    return result;
}
#define read ReadForTest
#define write WriteForTest
#define close CloseForTest
#include "dtcopy/fsrtns.c"
#undef read
#undef write
#undef close

int main(int argc, char **argv)
{
    assert(argc==2 && chdir(argv[1])==0);
    char expected[8193], actual[8193];
    for (size_t i=0; i<sizeof(expected); ++i) expected[i]=(char)(i%127);
    FILE *stream=fopen("source","wb"); assert(stream);
    assert(fwrite(expected,1,sizeof(expected),stream)==sizeof(expected));
    assert(fclose(stream)==0);
    struct stat info; assert(stat("source",&info)==0);
    for (scenario=0; scenario<=7; ++scenario) {
        reads=writes=0; source_fd=target_fd=-1;
        int result=CopyFile("source","target",0,&info);
        if (scenario<=2) {
            assert(result==0);
            stream=fopen("target","rb"); assert(stream);
            assert(fread(actual,1,sizeof(actual),stream)==sizeof(actual));
            assert(fgetc(stream)==EOF && !ferror(stream));
            assert(memcmp(actual,expected,sizeof(actual))==0);
            fclose(stream);
        } else {
            assert(result==(scenario==4 ? ENOSPC : EIO));
        }
        stream=fopen("source","rb"); assert(stream);
        assert(fread(actual,1,sizeof(actual),stream)==sizeof(actual));
        assert(memcmp(actual,expected,sizeof(actual))==0);
        fclose(stream);
        assert(unlink("target")==0);
    }
    assert(unlink("source")==0);
    puts("File copy: eight normal, short-I/O, interruption and error cases passed");
    return 0;
}
