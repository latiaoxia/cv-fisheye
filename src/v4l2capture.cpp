#include "v4l2capture.hpp"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <stdexcept>
#include <iostream>
#include <cstring>

namespace v4l2 {
Capture::~Capture()
{
    if (m_fd != -1)
        ::close(m_fd);
}

void Capture::open(const std::string &path, enum PixFormat pixFormat,
                   const std::vector<PixelBufferBase>& buffers)
{
    int ret;

    if (buffers.size() < 2 ||
        buffers[0].getWidth() <= 0 ||
        buffers[0].getHeight() <= 0) {
        throw std::runtime_error("invalid initialization params");
    }

    m_width = buffers[0].getWidth();
    m_height = buffers[0].getHeight();
    m_pixFmt = static_cast<uint32_t>(pixFormat);
    m_bufferNum = buffers.size();

    m_fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
    if (m_fd == -1) {
        throw std::runtime_error("failed to open: " + path);
    }

    struct v4l2_capability cap = {};
    ret = ioctl(m_fd, VIDIOC_QUERYCAP, &cap);
    if (ret == -1 || !(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE)) {
        throw std::runtime_error(path + ": do not support multi-plane");
    }

    enumFormat();

    struct v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = m_width;
    fmt.fmt.pix_mp.height = m_height;
    fmt.fmt.pix_mp.pixelformat = m_pixFmt;
    fmt.fmt.pix_mp.num_planes = 1;
    if (ioctl(m_fd, VIDIOC_S_FMT, &fmt)) {
        throw std::runtime_error("failed to set format");
    }

    std::memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(m_fd, VIDIOC_G_FMT, &fmt)) {
        throw std::runtime_error("VIDIOC_G_FMT failed");
    }
    std::cout << "\twidth: " << fmt.fmt.pix_mp.width
              << "\theight: " << fmt.fmt.pix_mp.height << std::endl;
    std::cout << "\timage size: " << fmt.fmt.pix_mp.plane_fmt[0].sizeimage
              << std::endl;
    std::cout << "\tpixelformat: "
              << static_cast<char>(fmt.fmt.pix_mp.pixelformat & 0xff)
              << static_cast<char>(fmt.fmt.pix_mp.pixelformat >> 8 & 0xff)
              << static_cast<char>(fmt.fmt.pix_mp.pixelformat >> 16 & 0xff)
              << static_cast<char>(fmt.fmt.pix_mp.pixelformat >> 24 & 0xff)
              << std::endl;
    m_frameSize =fmt.fmt.pix_mp.plane_fmt[0].sizeimage;

    struct v4l2_streamparm parm = {};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(m_fd, VIDIOC_G_PARM, &parm)) {
        throw std::runtime_error("failed to VIDIOC_G_PARM");
    }
    std::cout << "\tfps: " << parm.parm.capture.timeperframe.denominator
              << std::endl;

    struct v4l2_requestbuffers req = {};
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.count = m_bufferNum;
    req.memory = V4L2_MEMORY_USERPTR;
    if (ioctl(m_fd, VIDIOC_REQBUFS, &req)) {
        throw std::runtime_error("do not support V4L2_MEMORY_USERPTR");
    }
    if (req.count < 2) {
        throw std::runtime_error("Insufficient buffer memory");
    }

    m_buffers = buffers;

    for (int i = 0; i < m_buffers.size(); i++) {
        struct v4l2_buffer buf = {};
        struct v4l2_plane plane = {};

        plane.length = m_buffers.at(i).getLength();
        plane.m.userptr = reinterpret_cast<unsigned long>(m_buffers.at(i).getStart());

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_USERPTR;
        buf.index = i;
        buf.m.planes = &plane;
        buf.length = 1;

        if (ioctl(m_fd, VIDIOC_QBUF, &buf)) {
            std::cout << errno << std::endl;
            throw std::runtime_error("VIDIOC_QBUF error");
        }
    }
}

void Capture::start()
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(m_fd, VIDIOC_STREAMON, &type)) {
        throw std::runtime_error("VIDIOC_STREAMON error");
    }
}

void Capture::stop()
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    if (ioctl(m_fd, VIDIOC_STREAMOFF, &type)) {
        throw std::runtime_error("VIDIOC_STREAMOFF error");
    }
}

int Capture::readFrame()
{
    struct v4l2_buffer buf = {};
    struct v4l2_plane plane = {};

    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_USERPTR;
    buf.length = 1;
    buf.m.planes = &plane;

    if (ioctl(m_fd, VIDIOC_DQBUF, &buf)) {
        if (errno == EAGAIN)
            return -1;
        else
            throw std::runtime_error("VIDIOC_DQBUF err: " +
                                     std::to_string(errno));
    }

    return buf.index;
}

void Capture::doneFrame(int index)
{
    struct v4l2_buffer buf = {};
    struct v4l2_plane plane = {};

    plane.length = m_buffers.at(index).getLength();
    plane.m.userptr = reinterpret_cast<unsigned long>(m_buffers.at(index).getStart());

    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_USERPTR;
    buf.index = index;
    buf.m.planes = &plane;
    buf.length = 1;

    if (ioctl(m_fd, VIDIOC_QBUF, &buf)) {
        throw std::runtime_error("VIDIOC_QBUF error");
    }

}

std::shared_ptr<Buffer> Capture::dequeBuffer()
{
    int index = readFrame();

    if (index == -1)
        return std::make_shared<Buffer>();

    return std::make_shared<Buffer>(this, m_buffers[index]);
}

void Capture::enumFormat() const
{
    int ret;

    struct v4l2_fmtdesc fmtdesc = {};
    fmtdesc.type = V4L2_CAP_VIDEO_CAPTURE_MPLANE;
    for (int i = 0; ;i++) {
        fmtdesc.index = i;

        ret = ioctl(m_fd, VIDIOC_ENUM_FMT, &fmtdesc);
        if (ret == -1)
            break;

        std::cout << "index: " << i << ", pixelformat: "
                  << std::string(reinterpret_cast<char *>(&fmtdesc.pixelformat));
    }
}
}
