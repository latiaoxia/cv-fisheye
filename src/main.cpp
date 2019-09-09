#include <cstdio>
#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <map>

#include <signal.h>
#include <sys/epoll.h>
#include <sys/poll.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>

#include <opencv2/opencv.hpp>

#include "render.hpp"
#include "v4l2capture.hpp"
#include "replxx.hxx"
#include "message.hpp"

class RenderWorker
{
public:
struct Commit {};

    RenderWorker(Render& render_) :
        render(render_)
    {}

    messaging::Sender getSender()
    {
        return incoming;
    }

    void run()
    {
        try {
            for(;;) {
                incoming.wait()
                    .handle<std::shared_ptr<PixelBufferBase>>(
                        [&](std::shared_ptr<PixelBufferBase>& pbuf)
                        {
                            render.updateTexture(pbuf);
                        }
                    )
                    .handle<Commit>(
                        [&](Commit&)
                        {
                            render.render(0);
                        }
                    );
            }
        } catch(messaging::CloseQueue&) {
        }
    }

    void done()
    {
        getSender().send(messaging::CloseQueue());
    }

private:
    Render& render;
    messaging::Receiver incoming;
    void (RenderWorker::*state)();
};

class CaptureWorker
{
public:
    struct PreviewAll {};

    struct PreviewOne
    {
        PreviewOne(int num_ = 0) :
            num(num_)
        {}

        int num;
    };

    CaptureWorker(std::vector<v4l2::Capture>& caps, enum v4l2::PixFormat pixFormat,
                  const std::vector<std::vector<PixelBufferBase>>& bufBank,
                  messaging::Sender render_) :
        captures(caps),
        render(render_)
    {
        for (size_t i = 0; i < captures.size(); i++) {
            captures[i].open("/dev/video" + std::to_string(i), pixFormat, bufBank[i]);
            captures[i].start();
        }
    }

    void run()
    {
        try {
            for (;;) {
                incoming.wait()
                    .handle<PreviewAll>(
                        [&](const PreviewAll&)
                        {
                            epoll_fd = epoll_create1(0);

                            for (size_t i = 0; i < captures.size(); i++) {
                                struct epoll_event event = {};
                                event.data.u32 = i;
                                event.events = EPOLLIN; // do not use edge trigger
                                int ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, captures[i].getFd(),
                                        &event);
                                if (ret == -1)
                                    throw std::runtime_error("EPOLL_CTL_ADD error");
                            }

                            while (incoming.empty()) {
                                std::vector<struct epoll_event> events(captures.size());
                                int nevent = epoll_wait(epoll_fd, events.data(), events.size(), -1);
                                if (nevent == -1) {
                                    if (errno != EINTR)
                                        throw std::runtime_error("epoll_wait error, erron: " + std::to_string(errno));
                                }

                                for (int i = 0; i < nevent; i++) {
                                    int data = events[i].data.u32;
                                    std::shared_ptr<PixelBufferBase> pb(captures[data].dequeBuffer());
                                    render.send(pb);
                                }
                                render.send(RenderWorker::Commit());
                            }

                            // do not stop, bug in kernel driver
                            // for (auto& m : captures) {
                                // m.stop();
                            // }
                            close(epoll_fd);
                        }
                    )
                    .handle<PreviewOne>(
                        [&](const PreviewOne& msg)
                        {
                            currentCapture = msg.num;
                            epoll_fd = epoll_create1(0);
                            {
                                // captures[currentCapture].start();
                                struct epoll_event event = {};
                                event.data.u32 = currentCapture;
                                event.events = EPOLLIN;
                                int ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, captures[currentCapture].getFd(), &event);
                                if (ret == -1)
                                    throw std::runtime_error("EPOLL_CTL_ADD error");
                            }

                            while (incoming.empty()) {
                                struct epoll_event event;
                                int nevent = epoll_wait(epoll_fd, &event, 1, -1);
                                if (nevent == -1) {
                                    if (errno != EINTR)
                                        throw std::runtime_error("epoll_wait error, erron: " + std::to_string(errno));
                                }

                                int data = event.data.u32;
                                std::shared_ptr<PixelBufferBase> pb(captures[data].dequeBuffer());
                                render.send(pb);
                                render.send(RenderWorker::Commit());
                            }

                            // do not stop, bug in kernel driver
                            // captures[currentCapture].stop();
                            close(epoll_fd);
                        }
                    );
            }
        } catch (messaging::CloseQueue&) {
        }
    }

    void done()
    {
        getSender().send(messaging::CloseQueue());
    }

    messaging::Sender getSender()
    {
        return incoming;
    }

private:
    std::vector<v4l2::Capture>& captures;
    int epoll_fd;
    int currentCapture = 0;

    messaging::Receiver incoming;
    messaging::Sender render;
    messaging::Sender calibrator;
    void (CaptureWorker::*state)();
};

#if 0
class CliWorker
{
struct Input
{
    std::string str;
    Input(const std::string str_) :
        str(str_)
    {}
};

public:
    CliWorker(replxx::Replxx& rx_, messaging::Sender capture_) :
        rx(rx_),
        capture(capture_)
    {
    }

    void run()
    {
        state = &CliWorker::intrinsicStep;
        try {
            for (;;) {
                (this->*state)();
            }
        } catch (messaging::CloseQueue&) {
        }
    }

    void done()
    {
        getSender().send(messaging::CloseQueue());
    }

    messaging::Sender getSender()
    {
        return incoming;
    }

private:
    void intrinsicStep()
    {
        capture.send(v4l2::PreviewAll());
        incoming.wait()
            .handle<Input>(
                [&](const Input& in)
                {
                    capture.send();
                    state = &CliWorker::calibOneStep;
                }
            );
    }

    void calibOneStep()
    {
        // capture.send(messaging::);
        // incoming.wait()
            // .handle<>(
            // );
    }

    replxx::Replxx& rx;
    messaging::Receiver incoming;
    messaging::Sender capture;

    void (CliWorker::*state)();
};
#endif

// namespace menu
// {

// struct Pack
// {
    // std::string help;

// };

// struct MsgPack
// {
    // std::map<std::string>
// };

// }

int main()
{
    int cameraNum = 4;
    int qBufNum = 4;
    int imgWidth = 1280;
    int imgHeight = 800;
    enum v4l2::PixFormat pixelFmt = v4l2::PixFormat::XBGR32;
    int pixelSize = pixelFmt == v4l2::PixFormat::XBGR32 ? 4 : 4;
    int imgSize = imgHeight * imgWidth * pixelSize;

    // signal(SIGINT, [](int){ keepRunning = false; });
    std::vector<v4l2::Capture> captures(cameraNum);

    try {

        Render render(imgWidth, imgHeight, pixelSize, cameraNum, qBufNum);
        render.init();
        std::vector<std::vector<PixelBufferBase>> bufBank = render.getBufferBank();

        RenderWorker renderWorker(render);
        CaptureWorker capWorker(captures, pixelFmt, bufBank, renderWorker.getSender());

        // cmdline interface
        using namespace std::placeholders;
        using Replxx = replxx::Replxx;
        std::string input;

        replxx::Replxx rx;
        rx.install_window_change_handler();

        rx.set_max_history_size(128);
        rx.set_max_hint_rows(3);
        rx.bind_key( Replxx::KEY::BACKSPACE, std::bind( &Replxx::invoke, &rx, Replxx::ACTION::DELETE_CHARACTER_LEFT_OF_CURSOR, _1 ) );
        rx.bind_key( Replxx::KEY::DELETE, std::bind( &Replxx::invoke, &rx, Replxx::ACTION::DELETE_CHARACTER_UNDER_CURSOR, _1 ) );
        rx.bind_key( Replxx::KEY::LEFT, std::bind( &Replxx::invoke, &rx, Replxx::ACTION::MOVE_CURSOR_LEFT, _1 ) );
        rx.bind_key( Replxx::KEY::RIGHT, std::bind( &Replxx::invoke, &rx, Replxx::ACTION::MOVE_CURSOR_RIGHT, _1 ) );
        rx.bind_key( Replxx::KEY::UP, std::bind( &Replxx::invoke, &rx, Replxx::ACTION::HISTORY_PREVIOUS, _1 ) );
        rx.bind_key( Replxx::KEY::DOWN, std::bind( &Replxx::invoke, &rx, Replxx::ACTION::HISTORY_NEXT, _1 ) );
        rx.bind_key( Replxx::KEY::PAGE_UP, std::bind( &Replxx::invoke, &rx, Replxx::ACTION::HISTORY_FIRST, _1 ) );
        rx.bind_key( Replxx::KEY::PAGE_DOWN, std::bind( &Replxx::invoke, &rx, Replxx::ACTION::HISTORY_LAST, _1 ) );
        rx.bind_key( Replxx::KEY::HOME, std::bind( &Replxx::invoke, &rx, Replxx::ACTION::MOVE_CURSOR_TO_BEGINING_OF_LINE, _1 ) );
        rx.bind_key( Replxx::KEY::END, std::bind( &Replxx::invoke, &rx, Replxx::ACTION::MOVE_CURSOR_TO_END_OF_LINE, _1 ) );
        rx.bind_key( Replxx::KEY::TAB, std::bind( &Replxx::invoke, &rx, Replxx::ACTION::COMPLETE_LINE, _1 ) );
        std::cout
            << "Welcome to Replxx\n"
            << "Press 'tab' to view autocompletions\n"
            << "Type '.help' for help\n"
            << "Type '.quit' or '.exit' to exit\n\n";

        std::string prompt {"\x1b[1;32mreplxx\x1b[0m> "};

        // run thread
        std::thread renderThread(&RenderWorker::run, &renderWorker);
        std::thread captureThread(&CaptureWorker::run, &capWorker);
        messaging::Sender capQueue(capWorker.getSender());
        capQueue.send(CaptureWorker::PreviewAll());
        // capQueue.send(CaptureWorker::PreviewOne(0));

        std::string help1(std::string("input number: 0 to ") + std::to_string(captures.size() - 1) + " to select device");

        std::string help2(std::string("Input:\n") +
                          "\t\'b\': back");

        const std::string* phelp = &help1;

        std::function<void(void)>* stage;
        std::function<void(void)> f2;

        std::function<void(void)> f1 =
            [&]()
            {
                int num;
                try {
                    num = std::stoi(input);

                    if (num >= captures.size()) {
                        throw(num);
                    }

                    capQueue.send(CaptureWorker::PreviewOne(num));
                    stage = &f2;
                    phelp = &help2;
                } catch (...) {
                    std::cout << "invalid input" << std::endl;
                }
            };

        f2 =
            [&]()
            {
                if (input.compare("b") == 0) {
                    capQueue.send(CaptureWorker::PreviewAll());
                    stage = &f1;
                    phelp = &help2;
                } else {
                    std::cout << "invalid input" << std::endl;
                }
            };
        stage = &f1;

        for (;;) {
            char const* cinput{ nullptr };

            std::cout << *phelp << std::endl;

            do {
                cinput = rx.input(prompt);
            } while ( ( cinput == nullptr ) && ( errno == EAGAIN ) );

            if (cinput == nullptr) {
                break;
            }

            // change cinput into a std::string
            // easier to manipulate
            // std::string input {cinput};
            input = std::string(cinput);

            if (input.empty()) {
                // user hit enter on an empty line

                continue;

            } else if (input.compare(0, 5, ".quit") == 0 || input.compare(0, 5, ".exit") == 0) {
                // exit the repl

                rx.history_add(input);
                break;

            } else if (input.compare(0, 5, ".help") == 0) {
                // display the help output
                std::cout
                    << ".help\n\tdisplays the help output\n"
                    << ".quit\n\texit the repl\n"
                    << ".exit\n\texit the repl\n"
                    << ".clear\n\tclears the screen\n"
                    << ".history\n\tdisplays the history output\n"
                    << ".prompt <str>\n\tset the repl prompt to <str>\n";

                rx.history_add(input);
                continue;

            } else if (input.compare(0, 7, ".prompt") == 0) {
                // set the repl prompt text
                auto pos = input.find(" ");
                if (pos == std::string::npos) {
                    std::cout << "Error: '.prompt' missing argument\n";
                } else {
                    prompt = input.substr(pos + 1) + " ";
                }

                rx.history_add(input);
                continue;

            } else if (input.compare(0, 8, ".history") == 0) {
                // display the current history
                for (size_t i = 0, sz = rx.history_size(); i < sz; ++i) {
                    std::cout << std::setw(4) << i << ": " << rx.history_line(i) << "\n";
                }

                rx.history_add(input);
                continue;

            } else if (input.compare(0, 6, ".clear") == 0) {
                // clear the screen
                rx.clear_screen();

                rx.history_add(input);
                continue;

            } else {
                // default action

                (*stage)();

                rx.history_add( input );
                continue;
            }
        }

        capWorker.done();
        renderWorker.done();
        captureThread.join();
        renderThread.join();
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return -1;
    }

    return 0;
}
