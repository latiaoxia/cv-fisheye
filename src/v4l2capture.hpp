#pragma once

#include <linux/videodev2.h>
#include <string>
#include <vector>
#include <array>

#include "message.hpp"

namespace v4l2 {
    struct Start {};

    enum class PixFormat
    {
        XBGR32 = V4L2_PIX_FMT_XBGR32,
    };

    class Buffer;
    class Capture
    {
    public:
        Capture() = default;
        Capture(const Capture&) = delete;
        Capture& operator=(const Capture&) = delete;
        virtual ~Capture();

        void open(const std::string &path, enum PixFormat pixFormat,
                  const std::vector<PixelBufferBase>& buffers);
        void start();
        void stop();
        int readFrame();
        void doneFrame(int index);
        int getFd() const { return m_fd; }
        Buffer dequeBuffer();

    private:
        int m_fd = -1;
        int m_width;
        int m_height;
        int m_frameSize;
        uint32_t m_pixFmt;
        int m_bufferNum;
        std::vector<PixelBufferBase> m_buffers;

        void enumFormat() const;
    };

    class Buffer : public PixelBufferBase
    {
    public:
        Buffer(const Buffer&) = delete;
        Buffer& operator=(const Buffer&) = delete;

        Buffer() = default;

        Buffer(Capture* cap_, const PixelBufferBase& bufBase) :
            cap(cap_),
            PixelBufferBase(bufBase)
        {
        }

        Buffer(Buffer&& other) :
            PixelBufferBase(other),
            cap(std::exchange(other.cap, nullptr))
        {
        }

        Buffer& operator = (Buffer&& other)
        {
            if (this != &other) {
                release();
                cap = std::exchange(other.cap, nullptr);
            }

            return *this;
        }

        ~Buffer()
        {
            release();
        }

    private:
        Capture* cap = nullptr;

        void release()
        {
            if (cap)
                cap->doneFrame(getSubIndex());
            cap = nullptr;
        }
    };

}
