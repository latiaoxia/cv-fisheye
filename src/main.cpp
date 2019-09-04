#include <cstdio>
#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>

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

class Tick {
	typedef std::vector<char32_t> keys_t;
	std::thread _thread;
	int _tick;
	bool _alive;
	keys_t _keys;
	replxx::Replxx& _replxx;

public:
	Tick( replxx::Replxx& replxx_, std::string const& keys_ = {} )
		: _thread()
		, _tick( 0 )
		, _alive( false )
		, _keys( keys_.begin(), keys_.end() )
		, _replxx( replxx_ ) {
	}
	void start() {
		_alive = true;
		_thread = std::thread( &Tick::run, this );
	}
	void stop() {
		_alive = false;
		_thread.join();
	}
	void run() {
		std::string s;
		while ( _alive ) {
			if ( _keys.empty() ) {
				_replxx.print( "%d\n", _tick );
			} else if ( _tick < static_cast<int>( _keys.size() ) ) {
				_replxx.emulate_key_press( _keys[_tick] );
			} else {
				break;
			}
			++ _tick;
			std::this_thread::sleep_for( std::chrono::seconds( 1 ) );
		}
	}
};

static volatile bool keepRunning = true;

class CaptureWorker
{
public:
    CaptureWorker(std::vector<v4l2::Capture>& caps, enum v4l2::PixFormat pixFormat,
                  const std::vector<std::vector<PixelBufferBase>>& bufBank,
                  messaging::Sender render_) :
        captures(caps),
        render(render_)
    {
        for (size_t i = 0; i < captures.size(); i++) {
            captures[i].open("/dev/video" + std::to_string(i), pixFormat,
                             bufBank[i]);
            captures[i].start();
        }
    }

    void run()
    {
        state = &CaptureWorker::captureAll;
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
    void captureAll()
    {
        incoming.wait()
            .handle<v4l2::Start>(
                [&](const v4l2::Start&)
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
                            v4l2::Buffer buf(captures[data].dequeBuffer());
                            render.send(std::move(static_cast<PixelBufferBase>(buf)));
                        }
                        render.send(Render::Commit());
                    }
                    close(epoll_fd);
                }
            );
    }

    std::vector<v4l2::Capture>& captures;
    int epoll_fd;

    messaging::Receiver incoming;
    messaging::Sender render;
    messaging::Sender calibrator;
    void (CaptureWorker::*state)();
};

class RenderWorker
{
public:
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
                    .handle<PixelBufferBase>(
                        [&](PixelBufferBase& buf)
                        {
                            render.updateTexture(buf);
                        }
                    )
                    .handle<Render::Commit>(
                        [&](Render::Commit&)
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

int main()
{
    int cameraNum = 4;
    int qBufNum = 4;
    int imgWidth = 1280;
    int imgHeight = 800;
    enum v4l2::PixFormat pixelFmt = v4l2::PixFormat::XBGR32;
    int pixelSize = pixelFmt == v4l2::PixFormat::XBGR32 ? 4 : 4;
    int imgSize = imgHeight * imgWidth * pixelSize;

    signal(SIGINT, [](int){ keepRunning = false; });
    std::vector<v4l2::Capture> captures(cameraNum);

    try {
        using Replxx = replxx::Replxx;
        using namespace std::placeholders;
        std::string input;
        char cinput[512];
        int cread;

#if 0
        replxx::Replxx rx;
        Tick tick(rx);
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

        for (;;) {
            char const* cinput{ nullptr };

            do {
                cinput = rx.input(prompt);
            } while ( ( cinput == nullptr ) && ( errno == EAGAIN ) );

            if (cinput == nullptr) {
                break;
            }

            // change cinput into a std::string
            // easier to manipulate
            std::string input {cinput};

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
                // echo the input

                rx.print( "%s\n", input.c_str() );

                rx.history_add( input );
                continue;
            }
        }
#endif

        Render render(imgWidth, imgHeight, pixelSize, cameraNum, qBufNum);
        render.init();
        std::vector<std::vector<PixelBufferBase>> bufBank = render.getBufferBank();

        RenderWorker renderWorker(render);
        CaptureWorker capWorker(captures, pixelFmt, bufBank, renderWorker.getSender());

        std::thread renderThread(&RenderWorker::run, &renderWorker);
        std::thread captureThread(&CaptureWorker::run, &capWorker);
        messaging::Sender capQueue(capWorker.getSender());
        capQueue.send(v4l2::Start());

        while (keepRunning) {
             std::this_thread::sleep_for(std::chrono::seconds(1));
        };

        capWorker.done();
        renderWorker.done();
        captureThread.join();
        renderThread.join();

#if 0
        for (size_t i = 0; i < captures.size(); i++) {
            captures[i].open("/dev/video" + std::to_string(i), pixelFmt,
                             bufBank[i]);
            captures[i].start();

            struct epoll_event event = {};
            event.data.u32 = i;
            event.events = EPOLLIN; // do not use edge trigger
            int ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, captures[i].getFd(),
                    &event);
            if (ret == -1) {
                throw std::runtime_error("EPOLL_CTL_ADD error");
            }
        }

        int frameCount = 0;
        double previousTime = glfwGetTime();
        double currentTime;

        int index[4] = {0, 0, 0, 0};

        struct epoll_event events[5];
        int nevent;

        // add stdin to epoll
        struct epoll_event event;
        event.data.u32 = 4;
        event.events = EPOLLIN;
        fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, 0, &event)) {
            throw std::runtime_error("EPOLL_CTL_ADD err: " +
                                     std::to_string(errno));
        }

        while (keepRunning) {
            glfwPollEvents();

            nevent = epoll_wait(epoll_fd, events, 5, -1);
            if (nevent == -1) {
                if (errno == EINTR)
                    break;
                throw std::runtime_error("epoll_wait error, erron: " + std::to_string(errno));
            }

            for (int i = 0; i < nevent; i++) {
                int data = events[i].data.u32;
                switch (data) {
                case 0:
                case 1:
                case 2:
                case 3:
                {
                    v4l2::Buffer buf(captures[data].dequeBuffer());
                    render.updateTexture(buf);
                    break;
                }
                case 4:
                    cread = read(0, cinput, sizeof(cinput));
                    cinput[cread] = '\0';
                    printf("%d %s", cread, cinput);
                    // std::cout << cinput << std::endl;
                    break;
                default:
                    std::cout << "unknow event" << std::endl;
                    break;
                }
            }

            render.render(0);

            currentTime = glfwGetTime();
            frameCount++;
            double deltaT = currentTime - previousTime;
            if (deltaT >= 1.0) {
                // std::cout << "frame: " << frameCount / deltaT << std::endl;
                frameCount = 0;
                previousTime = currentTime;
            }
        }
#endif
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return -1;
    }

    return 0;
}
