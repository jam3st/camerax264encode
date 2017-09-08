// 	http://git.videolan.org/git/x264.git 
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <linux/videodev2.h>
#include <turbojpeg.h>
#include <x264.h>

#define CLEAR(x) memset(&(x), 0, sizeof(x))


struct buffer {
        void   *start;
        size_t  length;
};

static char            *dev_name;
static int              fd = -1;
struct buffer          *buffers;
static unsigned int     n_buffers;
static int              out_buf;
static int              force_format;
static int              frame_count = 100;
static int              frame_number = 0;
static tjhandle         jpegDecompressor = nullptr;
static constexpr auto width = 1280u;
static constexpr auto height = 720u;
x264_t *h;
x264_param_t param;
x264_picture_t pic;
FILE* fo = nullptr;

static void errno_exit(const char *s)
{
        fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));
        exit(1);
}

static int xioctl(int fh, int request, void *arg)
{
        int r;

        do {
                r = ioctl(fh, request, arg);
        } while (-1 == r && EINTR == errno);

        return r;
}

static void process_image(unsigned char *p, int size)
{
        unsigned char buff[width * height * 2];
        x264_picture_t pic_out;

        memset(&buff, 0, sizeof(buff));
//        tjDecompressHeader2(jpegDecompressor, p, size, &w, &h, &subsample);
//        printf("%d\n", subsample);
        //tjDecompressToYUV(jpegDecompressor, p, size, buff, TJFLAG_FASTDCT);
        pic.img.i_csp = X264_CSP_I422;
        pic.img.i_stride[0] = width ;
        pic.img.i_stride[1] = 640;
        pic.img.i_stride[2] = 640;
        pic.img.i_stride[3] = 0;
        tjDecompressToYUVPlanes(jpegDecompressor, p, size, pic.img.plane, width, pic.img.i_stride, height, TJFLAG_FASTDCT);
//        sprintf(filename, "F%05d.y", frame_number);
//        FILE *f=fopen(filename,"wb");
//        fwrite(buff, 1, sizeof(buff), f);
//        fclose(f);
        x264_nal_t* nals;
        int i_nals;
        int frame_size = x264_encoder_encode(h, &nals, &i_nals, &pic, &pic_out);
        if (frame_size >= 0)
        {
            printf("FS: %d %d\n", frame_size, frame_number);
            // OK
            if(frame_size > 0) {
              fwrite( nals->p_payload, frame_size, 1, fo);
            }
        }

}

static int read_frame(void)
{
        struct v4l2_buffer buf;
        unsigned int i;
            CLEAR(buf);

            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;

            if (-1 == xioctl(fd, VIDIOC_DQBUF, &buf)) {
                    switch (errno) {
                    case EAGAIN:
                            return 1;

                    case EIO:
                            /* Could ignore EIO, see spec. */

                            /* fall through */

                    default:
                            return -1;
                    }
            }

            assert(buf.index < n_buffers);
            process_image(static_cast<unsigned char*>(buffers[buf.index].start), buf.bytesused);

            if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
                    return -1;
            ++frame_number;
            if(frame_number > 300) {
                return -1;
            }
        return 0;
}

static void mainloop(void)
{
        unsigned int count;

        count = frame_count;

            for (;;) {
                    fd_set fds;
                    struct timeval tv;
                    int r;

                    FD_ZERO(&fds);
                    FD_SET(fd, &fds);

                    /* Timeout. */
                    tv.tv_sec = 2;
                    tv.tv_usec = 0;

                    r = select(fd + 1, &fds, NULL, NULL, &tv);

                    if (-1 == r) {
                            if (EINTR == errno)
                                    continue;
                            return;
                    }

                    if (0 == r) {
                            fprintf(stderr, "xselect timeout\n");
                            return;
                    }

                    if (read_frame() < 0)
                            break;
                    /* EAGAIN - continue select loop. */
        }
}

static void stop_capturing(void)
{
        enum v4l2_buf_type type;

        type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (-1 == xioctl(fd, VIDIOC_STREAMOFF, &type))
                errno_exit("VIDIOC_STREAMOFF");
}

static void start_capturing(void)
{
        unsigned int i;
        enum v4l2_buf_type type;
        for (i = 0; i < n_buffers; ++i) {
                struct v4l2_buffer buf;

                CLEAR(buf);
                buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                buf.memory = V4L2_MEMORY_MMAP;
                buf.index = i;

                if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
                        errno_exit("VIDIOC_QBUF");
        }
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (-1 == xioctl(fd, VIDIOC_STREAMON, &type))
                errno_exit("VIDIOC_STREAMON");
}

static void uninit_device(void)
{
        unsigned int i;

        for (i = 0; i < n_buffers; ++i)
                if (-1 == munmap(buffers[i].start, buffers[i].length))
                        errno_exit("munmap");
        free(buffers);
}


static void init_mmap(void)
{
        struct v4l2_requestbuffers req;

        CLEAR(req);

        req.count = 4;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;

        if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req)) {
                if (EINVAL == errno) {
                        fprintf(stderr, "%s does not support "
                                 "memory mapping\n", dev_name);
                        exit(EXIT_FAILURE);
                } else {
                        errno_exit("VIDIOC_REQBUFS");
                }
        }

        if (req.count < 2) {
                fprintf(stderr, "Insufficient buffer memory on %s\n",
                         dev_name);
                exit(EXIT_FAILURE);
        }

        buffers = static_cast<struct buffer*>(calloc(req.count, sizeof(*buffers)));

        if (!buffers) {
                fprintf(stderr, "Out of memory\n");
                exit(EXIT_FAILURE);
        }

        for (n_buffers = 0; n_buffers < req.count; ++n_buffers) {
                struct v4l2_buffer buf;

                CLEAR(buf);

                buf.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                buf.memory      = V4L2_MEMORY_MMAP;
                buf.index       = n_buffers;

                if (-1 == xioctl(fd, VIDIOC_QUERYBUF, &buf))
                        errno_exit("VIDIOC_QUERYBUF");

                buffers[n_buffers].length = buf.length;
                buffers[n_buffers].start =
                        mmap(NULL /* start anywhere */,
                              buf.length,
                              PROT_READ | PROT_WRITE /* required */,
                              MAP_SHARED,/* recommended */
                              fd, buf.m.offset);

                if (MAP_FAILED == buffers[n_buffers].start)
                        errno_exit("mmap");
        }
}


static void init_device(void)
{
        struct v4l2_capability cap;
        struct v4l2_cropcap cropcap;
        struct v4l2_crop crop;
        struct v4l2_format fmt;
        unsigned int min;

        if (-1 == xioctl(fd, VIDIOC_QUERYCAP, &cap)) {
                if (EINVAL == errno) {
                        fprintf(stderr, "%s is no V4L2 device\n",
                                 dev_name);
                        exit(EXIT_FAILURE);
                } else {
                        errno_exit("VIDIOC_QUERYCAP");
                }
        }

        if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
                fprintf(stderr, "%s is no video capture device\n",
                         dev_name);
                exit(EXIT_FAILURE);
        }

        if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
                fprintf(stderr, "%s does not support streaming i/o\n",
                         dev_name);
                exit(EXIT_FAILURE);
        }


        CLEAR(cropcap);

        cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        if (0 == xioctl(fd, VIDIOC_CROPCAP, &cropcap)) {
                crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                crop.c = cropcap.defrect; /* reset to default */

                if (-1 == xioctl(fd, VIDIOC_S_CROP, &crop)) {
                        switch (errno) {
                        case EINVAL:
                                /* Cropping not supported. */
                           break;
                        default:
                                  /* Errors ignored. */
                                  break;
                           }
                   }
           } else {
                  /* Errors ignored. */
           }


        CLEAR(fmt);

        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                fmt.fmt.pix.width       = width; //replace
                fmt.fmt.pix.height      = height; //replace
                fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG    ; //replace
                fmt.fmt.pix.field       = V4L2_FIELD_ANY;

                if (-1 == xioctl(fd, VIDIOC_S_FMT, &fmt))
                        errno_exit("VIDIOC_S_FMT");

                /* Note VIDIOC_S_FMT may change width and height. */

        /* Buggy driver paranoia. */
        init_mmap();
}

static void close_device(void)
{
        if (-1 == close(fd))
                errno_exit("close");

        fd = -1;
}

static void open_device(void)
{
        struct stat st;

        if (-1 == stat(dev_name, &st)) {
                fprintf(stderr, "Cannot identify '%s': %d, %s\n",
                         dev_name, errno, strerror(errno));
                exit(EXIT_FAILURE);
        }

        if (!S_ISCHR(st.st_mode)) {
                fprintf(stderr, "%s is no device\n", dev_name);
                exit(EXIT_FAILURE);
        }

        fd = open(dev_name, O_RDWR /* required */ | O_NONBLOCK, 0);

        if (-1 == fd) {
                fprintf(stderr, "Cannot open '%s': %d, %s\n",
                         dev_name, errno, strerror(errno));
                exit(EXIT_FAILURE);
        }
}



int main(int argc, char **argv)
{
    x264_picture_t pic_out;
    constexpr auto fps = 30u;
    if( x264_param_default_preset( &param, "superfast", NULL ) < 0 ) {
        fprintf(stderr, "Cannot identify x264_param_default_presets\n");
        exit(EXIT_FAILURE);
    }
    param.i_csp = X264_CSP_I422;
    param.i_width  = width;
    param.i_height = height;
    param.i_threads = 1;
    param.i_timebase_num = 1;
    param.i_timebase_den = fps;
    param.b_vfr_input = 0;
    param.b_repeat_headers = 1;
    param.b_annexb = 1;
    param.rc.i_rc_method = X264_RC_ABR;
    param.rc.i_bitrate = 1000000u;
    /* Apply profile restrictions. */
    if( x264_param_apply_profile( &param, "high422" ) < 0 ) {
        fprintf(stderr, "Cannot identify x264_param_apply_profile\n");
        exit(EXIT_FAILURE);
    }
    if( x264_picture_alloc( &pic, param.i_csp, param.i_width, param.i_height ) < 0 ) {
        fprintf(stderr, "Cannot identify x264_picture_alloc\n");
        exit(EXIT_FAILURE);
    }
    h = x264_encoder_open( &param );
    if( !h ) {
        fprintf(stderr, "Cannot identify x264_encoder_open\n");
        exit(EXIT_FAILURE);
    }
    fo = fopen("out.x264", "wb");
    jpegDecompressor = tjInitDecompress();
    dev_name = "/dev/video0";
    open_device();
    init_device();
    start_capturing();
    mainloop();
    stop_capturing();
    uninit_device();
    close_device();
    /* Flush delayed frames */
    while( x264_encoder_delayed_frames( h ) )
    {
        x264_nal_t* nals;
        int i_nals;
        auto frame_size = x264_encoder_encode( h, &nals, &i_nals, NULL, &pic_out );
        if( frame_size < 0 )
            exit(1);
        else if( frame_size )
        {
            if( !fwrite( nals->p_payload, frame_size, 1, fo ) )
               exit(1);
        }
    }


    x264_encoder_close( h );
    x264_picture_clean( &pic );
    fclose(fo);
    fprintf(stderr, "\n");
    return 0;
}

#if 0
int i_frame = 0;
int i_frame_size;
x264_nal_t *nal;
int i_nal;

    int luma_size = width * height;
    int chroma_size = luma_size / 4;
    /* Encode frames */
    for( ;; i_frame++ )
    {
        /* Read input frame */
        if( fread( pic.img.plane[0], 1, luma_size, stdin ) != luma_size )
            break;
        if( fread( pic.img.plane[1], 1, chroma_size, stdin ) != chroma_size )
            break;
        if( fread( pic.img.plane[2], 1, chroma_size, stdin ) != chroma_size )
            break;

        pic.i_pts = i_frame;
        i_frame_size = x264_encoder_encode( h, &nal, &i_nal, &pic, &pic_out );
        if( i_frame_size < 0 )
            goto fail;
        else if( i_frame_size )
        {
            if( !fwrite( nal->p_payload, i_frame_size, 1, stdout ) )
                goto fail;
        }
    }
    /* Flush delayed frames */
    while( x264_encoder_delayed_frames( h ) )
    {
        i_frame_size = x264_encoder_encode( h, &nal, &i_nal, NULL, &pic_out );
        if( i_frame_size < 0 )
            goto fail;
        else if( i_frame_size )
        {
            if( !fwrite( nal->p_payload, i_frame_size, 1, stdout ) )
                goto fail;
        }
    }
}
#endif
