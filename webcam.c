#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <linux/videodev2.h>

#define CLEAR(x) memset(&(x), 0, sizeof(x))

/**
 * Buffer structure
 */
struct buffer {
    char *start;
    size_t length;
};

/**
 * Webcam structure
 */
struct webcam {
    char            *name;
    int             fd;
    struct buffer   *buffers;
    unsigned int    nbuffers;
};

/**
 * Private function for successfully ioctl-ing the v4l2 device
 */
static int _ioctl(int fh, int request, void *arg)
{
    int r;

    do {
        r = ioctl(fh, request, arg);
    } while (-1 == r && EINTR == errno);

    return r;
}

/**
 * Private function to equalize the Y-histogram for contrast
 * using a cumulative distribution function
 *
 * http://en.wikipedia.org/wiki/Histogram_equalization
 */
static void equalize(struct buffer *buf)
{
    size_t i;
    unsigned int depth = 1 << 8;
    unsigned char value;

    size_t *histogram = calloc(depth, sizeof(size_t));
    size_t *cdf = calloc(depth, sizeof(size_t));
    size_t cdf_min = 0;

    // Skip CbCr components
    for (i = 0; i < buf->length; i += 2)
    {
        histogram[(unsigned char)buf->start[i]]++;
    }

    // Create cumulative distribution
    for (i = 0; i < depth; i++) {
        cdf[i] = 0 == i ? histogram[i] : cdf[i - 1] + histogram[i];
        if (cdf_min == 0 && cdf[i] > 0) cdf_min = cdf[i];
    }

    // Equalize the Y values
    for (i = 0; i < buf->length; i += 2) {
        value = (unsigned char)buf->start[i];
        buf->start[i] = 1.0 * (cdf[value] - cdf_min) / (buf->length / 2 - cdf_min) * (depth - 1);
    }
}

/**
 * Open the webcam on the given device and return a webcam
 * structure.
 */
struct webcam *webcam_open(const char *dev)
{
    struct stat st;

    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct v4l2_buffer buf;

    unsigned int min;

    int i;
    int fd;
    struct webcam *w;

    // Check if the device path exists
    if (-1 == stat(dev, &st)) {
        fprintf(stderr, "Cannot identify '%s': %d, %s\n",
                dev, errno, strerror(errno));
        return NULL;
    }

    // Should be a character device
    if (!S_ISCHR(st.st_mode)) {
        fprintf(stderr, "%s is no device\n", dev);
        return NULL;
    }

    // Create a file descriptor
    fd = open(dev, O_RDWR | O_NONBLOCK, 0);
    if (-1 == fd) {
        fprintf(stderr, "Cannot open'%s': %d, %s\n",
                dev, errno, strerror(errno));
        return NULL;
    }

    // Query the webcam capabilities
    if (-1 == _ioctl(fd, VIDIOC_QUERYCAP, &cap)) {
        if (EINVAL == errno) {
            fprintf(stderr, "%s is no V4L2 device\n", dev);
            return NULL;
        } else {
            fprintf(stderr, "%s: could not fetch video capabilities\n", dev);
            return NULL;
        }
    }

    // Needs to be a capturing device
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        fprintf(stderr, "%s is no video capture device\n", dev);
        return NULL;
    }

    // Prepare webcam structure
    w = calloc(1, sizeof(struct webcam));
    w->fd = fd;
    w->name = strdup(dev);

    // Request the webcam's buffers for memory-mapping
    struct v4l2_requestbuffers req;
    CLEAR(req);

    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (-1 == _ioctl(w->fd, VIDIOC_REQBUFS, &req)) {
        if (EINVAL == errno) {
            fprintf(stderr, "%s does not support memory mapping\n", w->name);
            return NULL;
        } else {
            fprintf(stderr, "Unknown error with VIDIOC_REQBUFS: %d\n", errno);
            return NULL;
        }
    }

    // Needs at least 2 buffers
    if (req.count < 2) {
        fprintf(stderr, "Insufficient buffer memory on %s\n", w->name);
        return NULL;
    }

    // Storing buffers in webcam structure
    fprintf(stderr, "Preparing %d buffers for %s\n", req.count, w->name);
    w->nbuffers = req.count;
    w->buffers = calloc(w->nbuffers, sizeof(struct buffer));

    if (!w->buffers) {
        fprintf(stderr, "Out of memory\n");
        return NULL;
    }

    // Prepare buffers to be memory-mapped
    for (i = 0; i < w->nbuffers; ++i) {
        CLEAR(buf);

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (-1 == _ioctl(w->fd, VIDIOC_QUERYBUF, &buf)) {
            fprintf(stderr, "Could not query buffers on %s\n", w->name);
            return NULL;
        }

        w->buffers[i].length = buf.length;
        w->buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, w->fd, buf.m.offset);

        if (MAP_FAILED == w->buffers[i].start) {
            fprintf(stderr, "Mmap failed\n");
            return NULL;
        }
    }

    // Query format of the capturing device
    CLEAR(fmt);
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (-1 == _ioctl(w->fd, VIDIOC_G_FMT, &fmt)) {
        fprintf(stderr, "%s: could not get image format\n", w->name);
    } else {
        char *pixelformat = calloc(5, sizeof(char));
        memcpy(pixelformat, &fmt.fmt.pix.pixelformat, 4);
        printf("%s: capturing (%d, %d) in %s:%d\n", dev, 
                fmt.fmt.pix.width, fmt.fmt.pix.height, pixelformat, fmt.fmt.pix.colorspace);
    }

    // Fix buggy drivers
    min = fmt.fmt.pix.width * 2;
    if (fmt.fmt.pix.bytesperline < min) fmt.fmt.pix.bytesperline = min;
    min = fmt.fmt.pix.bytesperline * fmt.fmt.pix.height;
    if (fmt.fmt.pix.sizeimage < min) fmt.fmt.pix.sizeimage = min;

    return w;
}

void webcam_read(struct webcam *w)
{
    int i;
    FILE *out1, *out2;
    char *fn1, *fn2;

    struct v4l2_buffer buf;
    enum v4l2_buf_type type;

    // Clear buffers
    for (i = 0; i < w->nbuffers; i++) {
        CLEAR(buf);
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (-1 == _ioctl(w->fd, VIDIOC_QBUF, &buf)) {
            fprintf(stderr, "Error clearing buffers on %s\n", w->name);
            return;
        }
    }

    // Turn on streaming
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (-1 == _ioctl(w->fd, VIDIOC_STREAMON, &type)) {
        fprintf(stderr, "Could not turn on streaming on %s\n", w->name);
        return;
    }

    // Prepare output file
    fn1 = calloc(15, sizeof(char));
    sprintf(fn1, "frame.yuv");
    out1 = fopen(fn1, "w+");

    fn2 = calloc(15, sizeof(char));
    sprintf(fn2, "equalized.yuv");
    out2 = fopen(fn2, "w+");

    // Try getting an image from the device
    for(;;) {
        CLEAR(buf);
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        // Dequeue a (filled) buffer from the video device
        if (-1 == _ioctl(w->fd, VIDIOC_DQBUF, &buf)) {
            switch(errno) {
                case EAGAIN:
                    continue;

                case EIO:
                default:
                    fprintf(stderr, "%d: Could not read from device %s\n", errno, w->name);
                    break;
            }
        }

        // Make sure we are not out of bounds
        assert(buf.index < w->nbuffers);

        // Save retrieved image
        fprintf(stderr, "Retrieved buffer %d of size %d (or %d) bytes from mmap\n", buf.index, buf.bytesused, buf.length);
        fwrite(w->buffers[buf.index].start, w->buffers[buf.index].length, 1, out1);
        fclose(out1);

        // Apply histogram stretching on the buffer in the Y channel
        fprintf(stderr, "Equalizing buffer\n");
        equalize(&(w->buffers[buf.index]));

        // Save resulting buffer into the output file
        fwrite(w->buffers[buf.index].start, w->buffers[buf.index].length, 1, out2);
        fclose(out2);
        break;
    }

    // Queue buffer back into the video device
    if (-1 == _ioctl(w->fd, VIDIOC_QBUF, &buf)) {
        fprintf(stderr, "Error while swapping buffers on %s\n", w->name);
        return;
    }

    // Turn off streaming
    if (-1 == _ioctl(w->fd, VIDIOC_STREAMOFF, &type)) {
        fprintf(stderr, "Could not turn streaming off on %s\n", w->name);
        return;
    }
}

/**
 * Main code
 */
int main(int argc, char **argv) {
    struct webcam *w = webcam_open("/dev/video0");

    webcam_read(w);

    return 0;
}
