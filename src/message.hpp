#pragma once

#include <mutex>
#include <condition_variable>
#include <queue>
#include <memory>

/*
 * refer to C++ Concurrency in Action, 2nd Edition appedix C
 *
 */

namespace messaging
{
    struct MessageBase
    {
        virtual ~MessageBase() {}
    };

    template<typename Msg>
    struct WrappedMessage : public MessageBase
    {
        Msg contents;
        WrappedMessage(const Msg& contents_) :
            contents(contents_)
        {}
    };

    class Queue
    {
    public:
        template<typename T>
        void push(const T& msg)
        {
            std::lock_guard<std::mutex> lk(m);
            q.push(std::make_shared<WrappedMessage<T>>(msg));
            c.notify_all();
        }

        std::shared_ptr<MessageBase> waitAndPop()
        {
            std::unique_lock<std::mutex> lk(m);
            c.wait(lk, [&]{ return !q.empty(); });
            auto res = q.front();
            q.pop();
            return res;
        }

        bool empty() const
        {
            std::lock_guard<std::mutex> lk(m);
            return q.empty();
        }

    private:
        mutable std::mutex m;
        std::condition_variable c;
        std::queue<std::shared_ptr<MessageBase>> q;
    };

    class CloseQueue
    {};

    template<typename PreviousDispatcher,typename Msg,typename Func>
    class TemplateDispatcher
    {
    public:
        TemplateDispatcher(TemplateDispatcher&& other) :
            q(other.q),
            prev(other.prev),
            f(std::move(other.f)),
            chained(other.chained)
        {
            other.chained = true;
        }

        TemplateDispatcher(Queue* q_, PreviousDispatcher* prev_, Func&& f_) :
            q(q_),
            prev(prev_),
            f(std::forward<Func>(f_)),
            chained(false)
        {
            prev_->chained = true;
        }

        template<typename OtherMsg,typename OtherFunc>
        TemplateDispatcher<TemplateDispatcher, OtherMsg, OtherFunc> handle(OtherFunc&& of)
        {
            return TemplateDispatcher<TemplateDispatcher, OtherMsg, OtherFunc>(
                    q, this, std::forward<OtherFunc>(of));
        }

        ~TemplateDispatcher() noexcept(false)
        {
            if (!chained) {
                waitAndDispatch();
            }
        }

    private:
        Queue* q;
        PreviousDispatcher* prev;
        Func f;
        bool chained;

        TemplateDispatcher(TemplateDispatcher const&) = delete;
        TemplateDispatcher& operator=(TemplateDispatcher const&) = delete;

        template<typename Dispatcher,typename OtherMsg,typename OtherFunc>
        friend class TemplateDispatcher;

        bool dispatch(std::shared_ptr<MessageBase> const& msg)
        {
            if (WrappedMessage<Msg>* wrapper =
                dynamic_cast<WrappedMessage<Msg>*>(msg.get())) {
                f(wrapper->contents);
                return true;
            } else {
                return prev->dispatch(msg);
            }
        }

        void waitAndDispatch()
        {
            for (;;) {
                auto msg = q->waitAndPop();
                if (dispatch(msg))
                    break;
            }
        }
    };

    class Dispatcher
    {
    public:
        Dispatcher(Dispatcher&& other) :
            q(other.q),
            chained(other.chained)
        {
            other.chained = true;
        }

        explicit Dispatcher(Queue* q_) :
            q(q_),
            chained(false)
        {}

        template<typename Message,typename Func>
        TemplateDispatcher<Dispatcher, Message, Func> handle(Func&& f)
        {
            return TemplateDispatcher<Dispatcher, Message, Func>(
                    q, this, std::forward<Func>(f));
        }

        ~Dispatcher() noexcept(false)
        {
            if (!chained) {
                waitAndDispatch();
            }
        }

    private:
        Queue *q;
        bool chained;

        Dispatcher(Dispatcher const&) = delete;
        Dispatcher& operator=(Dispatcher const&) = delete;

        template<typename Disp, typename Msg, typename Func>
        friend class TemplateDispatcher;

        bool dispatch(std::shared_ptr<MessageBase> const& msg)
        {
            if (dynamic_cast<WrappedMessage<CloseQueue>*>(msg.get())) {
                throw CloseQueue();
            }

            return false;
        }

        void waitAndDispatch()
        {
            for (;;) {
                auto msg = q->waitAndPop();
                dispatch(msg);
            }
        }
    };

    class Sender
    {
    public:
        Sender() :
            q(nullptr)
        {}

        explicit Sender(Queue *q_) :
            q(q_)
        {}

        template<typename Msg>
        void send(const Msg& msg)
        {
            if(q) {
                q->push(msg);
            }
        }

    private:
        Queue* q;
    };

    class Receiver
    {
    public:
        operator Sender()
        {
            return Sender(&q);
        }

        Dispatcher wait()
        {
            return Dispatcher(&q);
        }

        bool empty() const
        {
            return q.empty();
        }

    private:
        Queue q;
    };

}

class PixelBufferBase
{
public:
    PixelBufferBase(void* start_, size_t length_, int width_, int height_, int index_, int subIndex_) :
        start(start_),
        length(length_),
        width(width_),
        height(height_),
        index(index_),
        subIndex(subIndex_)
    {}

    PixelBufferBase() = default;

    void* getStart() const { return start; }
    size_t getLength() const { return length; }
    int getWidth() const { return width; }
    int getHeight() const { return height; }
    int getIndex() const { return index; }
    int getSubIndex() const { return subIndex; }

    virtual ~PixelBufferBase()
    {}

protected:
    void* start = nullptr;
    size_t length = 0;
    int width = 0;
    int height = 0;
    int index = 0;
    int subIndex = 0;
};

