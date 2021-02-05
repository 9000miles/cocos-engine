/****************************************************************************
 Copyright (c) 2021 Xiamen Yaji Software Co., Ltd.

 http://www.cocos.com

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated engine source code (the "Software"), a limited,
 worldwide, royalty-free, non-assignable, revocable and non-exclusive license
 to use Cocos Creator solely to develop games on your target platforms. You shall
 not use Cocos Creator software for developing other software or tools that's
 used for developing games. You are not granted to publish, distribute,
 sublicense, and/or sell copies of Cocos Creator.

 The software or tools in this License Agreement are licensed, not sold.
 Xiamen Yaji Software Co., Ltd. reserves all rights not expressly granted to you.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
****************************************************************************/

#pragma once

#include "Event.h"
#include "concurrentqueue/concurrentqueue.h"
#include <cstdint>

namespace cc {

// TODO: thread-specific allocators
template <typename T>
inline T *memoryAllocateForMultiThread(uint32_t const count) noexcept {
    return static_cast<T *>(malloc(sizeof(T) * count));
}

template <typename T>
inline void memoryFreeForMultiThread(T *const p) noexcept {
    free(p);
}

inline uint32_t constexpr align(uint32_t const val, uint32_t const alignment) noexcept {
    return (val + alignment - 1) & ~(alignment - 1);
}

class Message {
public:
    Message() = default;
    virtual ~Message() {}
    Message(Message const &) = delete;
    Message(Message &&) = delete;
    Message &operator=(Message const &) = delete;
    Message &operator=(Message &&) = delete;

    virtual void execute() noexcept = 0;
    virtual char const *getName() const noexcept = 0;
    inline Message *getNext() const noexcept { return mNext; }

private:
    Message *mNext;

    friend class MessageQueue;
};

struct alignas(64) WriterContext final {
    uint8_t *mCurrentMemoryChunk{nullptr};
    Message *mLastMessage{nullptr};
    uint32_t mOffset{0};
    uint32_t mPendingMessageCount{0};
    std::atomic<uint32_t> mWrittenMessageCount{0};
};

struct alignas(64) ReaderContext final {
    uint8_t *mCurrentMemoryChunk{nullptr};
    Message *mLastMessage{nullptr};
    uint32_t mOffset{0};
    uint32_t mWrittenMessageCountSnap{0};
    uint32_t mNewMessageCount{0};
    bool mTerminateConsumerThread{false};
    bool mFlushingFinished{false};
};

// A single-producer single-consumer circular buffer queue.
// Both the messages and their submitting data should be allocated from here.
class alignas(64) MessageQueue final {
public:
    MessageQueue();
    ~MessageQueue() = default;
    MessageQueue(MessageQueue const &) = delete;
    MessageQueue(MessageQueue &&) = delete;
    MessageQueue &operator=(MessageQueue const &) = delete;
    MessageQueue &operator=(MessageQueue &&) = delete;

    // message allocation
    template <typename T>
    std::enable_if_t<std::is_base_of<Message, T>::value, T *>
    allocate(uint32_t const count) noexcept;

    // general-purpose allocation
    template <typename T>
    std::enable_if_t<!std::is_base_of<Message, T>::value, T *>
    allocate(uint32_t const count) noexcept;
    template <typename T>
    T *allocateAndCopy(uint32_t const count, void const *data) noexcept;
    template <typename T>
    T *allocateAndZero(uint32_t const count) noexcept;

    // notify the consumer to start working
    void kick() noexcept;

    // notify the consumer to start working and block the producer until finished
    void kickAndWait() noexcept;

    void runConsumerThread() noexcept;
    void terminateConsumerThread() noexcept;
    void finishWriting(bool wait) noexcept;
    void flushMessages() noexcept;

    inline bool isImmediateMode() const noexcept { return mImmediateMode; }
    inline void finishWriting() noexcept { finishWriting(false); }

    void recycleMemoryChunk(uint8_t *const chunk) const noexcept;
    static void freeChunksInFreeQueue(MessageQueue *const mainMessagesQueue) noexcept;

    inline void setImmediateMode(bool immediateMode) noexcept { mImmediateMode = immediateMode; }
    inline int getPendingMessageCount() const noexcept { return mW.mPendingMessageCount; }
    inline int getWrittenMessageCount() const noexcept { return mW.mWrittenMessageCount; }
    inline int getNewMessageCount() const noexcept { return mR.mNewMessageCount; }

private:
    class alignas(64) MemoryAllocator final {
    public:
        MemoryAllocator() = default;
        ~MemoryAllocator() = default;
        MemoryAllocator(MemoryAllocator const &) = delete;
        MemoryAllocator(MemoryAllocator &&) = delete;
        MemoryAllocator &operator=(MemoryAllocator const &) = delete;
        MemoryAllocator &operator=(MemoryAllocator &&) = delete;

        static MemoryAllocator &getInstance() noexcept;
        uint8_t *request() noexcept;
        void recycle(uint8_t *const chunk, bool const freeByUser) noexcept;
        void freeByUser(MessageQueue *const mainMessageQueue) noexcept;

    private:
        using ChunkQueue = moodycamel::ConcurrentQueue<uint8_t *>;

        void free(uint8_t *const chunk) noexcept;
        std::atomic<uint32_t> mChunkCount{0};
        ChunkQueue mChunkPool{};
        ChunkQueue mChunkFreeQueue{};
    };

    uint8_t *allocateImpl(uint32_t &allocatedSize, uint32_t const requestSize) noexcept;
    void pushMessages() noexcept;

    // consumer thread specifics
    void pullMessages() noexcept;
    void executeMessages() noexcept;
    Message *readMessage() noexcept;
    inline bool hasNewMessage() const noexcept { return mR.mNewMessageCount > 0 && !mR.mFlushingFinished; }
    void consumerThreadLoop() noexcept;

    WriterContext mW;
    ReaderContext mR;
    EventCV mN;
    bool mImmediateMode{true};
    bool mWorkerAttached{false};
    bool mFreeChunksByUser{true}; // recycled chunks will be stashed until explicit free instruction

    friend class MemoryChunkSwitchMessage;
};

class DummyMessage final : public Message {
public:
    virtual void execute() noexcept override {}
    virtual char const *getName() const noexcept override;
};

class MemoryChunkSwitchMessage final : public Message {
public:
    MemoryChunkSwitchMessage(MessageQueue *const cb, uint8_t *const newChunk, uint8_t *const oldChunk) noexcept;
    ~MemoryChunkSwitchMessage();

    virtual void execute() noexcept override;
    virtual char const *getName() const noexcept override;

private:
    MessageQueue *mMessageQueue{nullptr};
    uint8_t *mNewChunk{nullptr};
    uint8_t *mOldChunk{nullptr};
};

class TerminateConsumerThreadMessage final : public Message {
public:
    TerminateConsumerThreadMessage(EventSem *const pEvent, ReaderContext *const pR) noexcept;

    virtual void execute() noexcept override;
    virtual char const *getName() const noexcept override;

private:
    EventSem *mEvent{nullptr};
    ReaderContext *mR{nullptr};
};

template <typename T>
std::enable_if_t<std::is_base_of<Message, T>::value, T *>
MessageQueue::allocate(uint32_t const count) noexcept {
    uint32_t allocatedSize = 0;
    T *const msg = reinterpret_cast<T *>(allocateImpl(allocatedSize, sizeof(T)));
    msg->mNext = reinterpret_cast<Message *>(mW.mCurrentMemoryChunk + mW.mOffset);
    ++mW.mPendingMessageCount;
    mW.mLastMessage = msg;
    return msg;
}

template <typename T>
std::enable_if_t<!std::is_base_of<Message, T>::value, T *>
MessageQueue::allocate(uint32_t const count) noexcept {
    uint32_t const requestSize = sizeof(T) * count;
    assert(requestSize);
    uint32_t allocatedSize = 0;
    uint8_t *const allocatedMemory = allocateImpl(allocatedSize, requestSize);
    mW.mLastMessage->mNext = reinterpret_cast<Message *>(mW.mCurrentMemoryChunk + mW.mOffset);
    return reinterpret_cast<T *>(allocatedMemory);
}

template <typename T>
T *MessageQueue::allocateAndCopy(uint32_t const count, void const *data) noexcept {
    T *const allocatedMemory = allocate<T>(count);
    memcpy(allocatedMemory, data, sizeof(T) * count);
    return allocatedMemory;
}

template <typename T>
T *MessageQueue::allocateAndZero(uint32_t const count) noexcept {
    T *const allocatedMemory = allocate<T>(count);
    memset(allocatedMemory, 0, sizeof(T) * count);
    return allocatedMemory;
}

// utility macros for the producer thread to enqueue messages

#define WRITE_MESSAGE(queue, MessageName, Params)                     \
    {                                                                 \
        if (!queue->isImmediateMode()) {                              \
            new (queue->allocate<MessageName>(1)) MessageName Params; \
        } else {                                                      \
            MessageName msg Params;                                   \
            msg.execute();                                            \
        }                                                             \
    }

#define ENQUEUE_MESSAGE_0(queue, MessageName, Code)                 \
    {                                                               \
        class MessageName final : public Message {                  \
        public:                                                     \
            virtual void execute() noexcept override {              \
                Code                                                \
            }                                                       \
            virtual char const *getName() const noexcept override { \
                return (#MessageName);                              \
            }                                                       \
        };                                                          \
        WRITE_MESSAGE(queue, MessageName, );                        \
    }

#define ENQUEUE_MESSAGE_1(queue, MessageName,                       \
                          Param1, Value1,                           \
                          Code)                                     \
    {                                                               \
                                                                    \
        using Type1 = std::decay<decltype(Value1)>::type;           \
                                                                    \
        class MessageName final : public Message {                  \
        public:                                                     \
            explicit MessageName(Type1 const &In##Param1)           \
            : Param1(In##Param1) {                                  \
            }                                                       \
            virtual void execute() noexcept override {              \
                Code                                                \
            }                                                       \
            virtual char const *getName() const noexcept override { \
                return (#MessageName);                              \
            }                                                       \
                                                                    \
        private:                                                    \
            Type1 Param1;                                           \
        };                                                          \
        WRITE_MESSAGE(queue, MessageName, (Value1))                 \
    }

#define ENQUEUE_MESSAGE_2(queue, MessageName,                       \
                          Param1, Value1,                           \
                          Param2, Value2,                           \
                          Code)                                     \
    {                                                               \
                                                                    \
        using Type1 = std::decay<decltype(Value1)>::type;           \
        using Type2 = std::decay<decltype(Value2)>::type;           \
                                                                    \
        class MessageName final : public Message {                  \
        public:                                                     \
            MessageName(                                            \
                Type1 const &In##Param1, Type2 const &In##Param2)   \
            : Param1(In##Param1), Param2(In##Param2) {              \
            }                                                       \
            virtual void execute() noexcept override {              \
                Code                                                \
            }                                                       \
            virtual char const *getName() const noexcept override { \
                return (#MessageName);                              \
            }                                                       \
                                                                    \
        private:                                                    \
            Type1 Param1;                                           \
            Type2 Param2;                                           \
        };                                                          \
        WRITE_MESSAGE(queue, MessageName, (Value1, Value2))         \
    }

#define ENQUEUE_MESSAGE_3(queue, MessageName,                       \
                          Param1, Value1,                           \
                          Param2, Value2,                           \
                          Param3, Value3,                           \
                          Code)                                     \
    {                                                               \
                                                                    \
        using Type1 = std::decay<decltype(Value1)>::type;           \
        using Type2 = std::decay<decltype(Value2)>::type;           \
        using Type3 = std::decay<decltype(Value3)>::type;           \
                                                                    \
        class MessageName final : public Message {                  \
        public:                                                     \
            MessageName(                                            \
                Type1 const &In##Param1,                            \
                Type2 const &In##Param2,                            \
                Type3 const &In##Param3)                            \
            : Param1(In##Param1),                                   \
              Param2(In##Param2),                                   \
              Param3(In##Param3) {                                  \
            }                                                       \
            virtual void execute() noexcept override {              \
                Code                                                \
            }                                                       \
            virtual char const *getName() const noexcept override { \
                return (#MessageName);                              \
            }                                                       \
                                                                    \
        private:                                                    \
            Type1 Param1;                                           \
            Type2 Param2;                                           \
            Type3 Param3;                                           \
        };                                                          \
        WRITE_MESSAGE(queue, MessageName,                           \
                      (Value1,                                      \
                       Value2,                                      \
                       Value3))                                     \
    }

#define ENQUEUE_MESSAGE_4(queue, MessageName,                       \
                          Param1, Value1,                           \
                          Param2, Value2,                           \
                          Param3, Value3,                           \
                          Param4, Value4,                           \
                          Code)                                     \
    {                                                               \
                                                                    \
        using Type1 = std::decay<decltype(Value1)>::type;           \
        using Type2 = std::decay<decltype(Value2)>::type;           \
        using Type3 = std::decay<decltype(Value3)>::type;           \
        using Type4 = std::decay<decltype(Value4)>::type;           \
                                                                    \
        class MessageName : public Message {                        \
        public:                                                     \
            MessageName(                                            \
                Type1 const &In##Param1,                            \
                Type2 const &In##Param2,                            \
                Type3 const &In##Param3,                            \
                Type4 const &In##Param4)                            \
            : Param1(In##Param1),                                   \
              Param2(In##Param2),                                   \
              Param3(In##Param3),                                   \
              Param4(In##Param4) {                                  \
            }                                                       \
            virtual void execute() noexcept override {              \
                Code                                                \
            }                                                       \
            virtual char const *getName() const noexcept override { \
                return (#MessageName);                              \
            }                                                       \
                                                                    \
        private:                                                    \
            Type1 Param1;                                           \
            Type2 Param2;                                           \
            Type3 Param3;                                           \
            Type4 Param4;                                           \
        };                                                          \
        WRITE_MESSAGE(queue, MessageName,                           \
                      (Value1,                                      \
                       Value2,                                      \
                       Value3,                                      \
                       Value4))                                     \
    }

#define ENQUEUE_MESSAGE_5(queue, MessageName,                       \
                          Param1, Value1,                           \
                          Param2, Value2,                           \
                          Param3, Value3,                           \
                          Param4, Value4,                           \
                          Param5, Value5,                           \
                          Code)                                     \
    {                                                               \
                                                                    \
        using Type1 = std::decay<decltype(Value1)>::type;           \
        using Type2 = std::decay<decltype(Value2)>::type;           \
        using Type3 = std::decay<decltype(Value3)>::type;           \
        using Type4 = std::decay<decltype(Value4)>::type;           \
        using Type5 = std::decay<decltype(Value5)>::type;           \
                                                                    \
        class MessageName : public Message {                        \
        public:                                                     \
            MessageName(                                            \
                Type1 const &In##Param1,                            \
                Type2 const &In##Param2,                            \
                Type3 const &In##Param3,                            \
                Type4 const &In##Param4,                            \
                Type5 const &In##Param5)                            \
            : Param1(In##Param1),                                   \
              Param2(In##Param2),                                   \
              Param3(In##Param3),                                   \
              Param4(In##Param4),                                   \
              Param5(In##Param5) {                                  \
            }                                                       \
            virtual void execute() noexcept override {              \
                Code                                                \
            }                                                       \
            virtual char const *getName() const noexcept override { \
                return (#MessageName);                              \
            }                                                       \
                                                                    \
        private:                                                    \
            Type1 Param1;                                           \
            Type2 Param2;                                           \
            Type3 Param3;                                           \
            Type4 Param4;                                           \
            Type5 Param5;                                           \
        };                                                          \
        WRITE_MESSAGE(queue, MessageName,                           \
                      (Value1,                                      \
                       Value2,                                      \
                       Value3,                                      \
                       Value4,                                      \
                       Value5))                                     \
    }

#define ENQUEUE_MESSAGE_6(queue, MessageName,                       \
                          Param1, Value1,                           \
                          Param2, Value2,                           \
                          Param3, Value3,                           \
                          Param4, Value4,                           \
                          Param5, Value5,                           \
                          Param6, Value6,                           \
                          Code)                                     \
    {                                                               \
                                                                    \
        using Type1 = std::decay<decltype(Value1)>::type;           \
        using Type2 = std::decay<decltype(Value2)>::type;           \
        using Type3 = std::decay<decltype(Value3)>::type;           \
        using Type4 = std::decay<decltype(Value4)>::type;           \
        using Type5 = std::decay<decltype(Value5)>::type;           \
        using Type6 = std::decay<decltype(Value6)>::type;           \
                                                                    \
        class MessageName : public Message {                        \
        public:                                                     \
            MessageName(                                            \
                Type1 const &In##Param1,                            \
                Type2 const &In##Param2,                            \
                Type3 const &In##Param3,                            \
                Type4 const &In##Param4,                            \
                Type5 const &In##Param5,                            \
                Type6 const &In##Param6)                            \
            : Param1(In##Param1),                                   \
              Param2(In##Param2),                                   \
              Param3(In##Param3),                                   \
              Param4(In##Param4),                                   \
              Param5(In##Param5),                                   \
              Param6(In##Param6) {                                  \
            }                                                       \
            virtual void execute() noexcept override {              \
                Code                                                \
            }                                                       \
            virtual char const *getName() const noexcept override { \
                return (#MessageName);                              \
            }                                                       \
                                                                    \
        private:                                                    \
            Type1 Param1;                                           \
            Type2 Param2;                                           \
            Type3 Param3;                                           \
            Type4 Param4;                                           \
            Type5 Param5;                                           \
            Type6 Param6;                                           \
        };                                                          \
        WRITE_MESSAGE(queue, MessageName,                           \
                      (Value1,                                      \
                       Value2,                                      \
                       Value3,                                      \
                       Value4,                                      \
                       Value5,                                      \
                       Value6))                                     \
    }

#define ENQUEUE_MESSAGE_7(queue, MessageName,                       \
                          Param1, Value1,                           \
                          Param2, Value2,                           \
                          Param3, Value3,                           \
                          Param4, Value4,                           \
                          Param5, Value5,                           \
                          Param6, Value6,                           \
                          Param7, Value7,                           \
                          Code)                                     \
    {                                                               \
                                                                    \
        using Type1 = std::decay<decltype(Value1)>::type;           \
        using Type2 = std::decay<decltype(Value2)>::type;           \
        using Type3 = std::decay<decltype(Value3)>::type;           \
        using Type4 = std::decay<decltype(Value4)>::type;           \
        using Type5 = std::decay<decltype(Value5)>::type;           \
        using Type6 = std::decay<decltype(Value6)>::type;           \
        using Type7 = std::decay<decltype(Value7)>::type;           \
                                                                    \
        class MessageName : public Message {                        \
        public:                                                     \
            MessageName(                                            \
                Type1 const &In##Param1,                            \
                Type2 const &In##Param2,                            \
                Type3 const &In##Param3,                            \
                Type4 const &In##Param4,                            \
                Type5 const &In##Param5,                            \
                Type6 const &In##Param6,                            \
                Type7 const &In##Param7)                            \
            : Param1(In##Param1),                                   \
              Param2(In##Param2),                                   \
              Param3(In##Param3),                                   \
              Param4(In##Param4),                                   \
              Param5(In##Param5),                                   \
              Param6(In##Param6),                                   \
              Param7(In##Param7) {                                  \
            }                                                       \
            virtual void execute() noexcept override {              \
                Code                                                \
            }                                                       \
            virtual const char *getName() const noexcept override { \
                return (#MessageName);                              \
            }                                                       \
                                                                    \
        private:                                                    \
            Type1 Param1;                                           \
            Type2 Param2;                                           \
            Type3 Param3;                                           \
            Type4 Param4;                                           \
            Type5 Param5;                                           \
            Type6 Param6;                                           \
            Type7 Param7;                                           \
        };                                                          \
        WRITE_MESSAGE(queue, MessageName,                           \
                      (Value1,                                      \
                       Value2,                                      \
                       Value3,                                      \
                       Value4,                                      \
                       Value5,                                      \
                       Value6,                                      \
                       Value7))                                     \
    }

#define ENQUEUE_MESSAGE_8(queue, MessageName,                       \
                          Param1, Value1,                           \
                          Param2, Value2,                           \
                          Param3, Value3,                           \
                          Param4, Value4,                           \
                          Param5, Value5,                           \
                          Param6, Value6,                           \
                          Param7, Value7,                           \
                          Param8, Value8,                           \
                          Code)                                     \
    {                                                               \
                                                                    \
        using Type1 = std::decay<decltype(Value1)>::type;           \
        using Type2 = std::decay<decltype(Value2)>::type;           \
        using Type3 = std::decay<decltype(Value3)>::type;           \
        using Type4 = std::decay<decltype(Value4)>::type;           \
        using Type5 = std::decay<decltype(Value5)>::type;           \
        using Type6 = std::decay<decltype(Value6)>::type;           \
        using Type7 = std::decay<decltype(Value7)>::type;           \
        using Type8 = std::decay<decltype(Value8)>::type;           \
                                                                    \
        class MessageName : public Message {                        \
        public:                                                     \
            MessageName(Type1 const &In##Param1,                    \
                        Type2 const &In##Param2,                    \
                        Type3 const &In##Param3,                    \
                        Type4 const &In##Param4,                    \
                        Type5 const &In##Param5,                    \
                        Type6 const &In##Param6,                    \
                        Type7 const &In##Param7,                    \
                        Type8 const &In##Param8)                    \
            : Param1(In##Param1),                                   \
              Param2(In##Param2),                                   \
              Param3(In##Param3),                                   \
              Param4(In##Param4),                                   \
              Param5(In##Param5),                                   \
              Param6(In##Param6),                                   \
              Param7(In##Param7),                                   \
              Param8(In##Param8) {                                  \
            }                                                       \
            virtual void execute() noexcept override {              \
                Code                                                \
            }                                                       \
            virtual char const *getName() const noexcept override { \
                return (#MessageName);                              \
            }                                                       \
                                                                    \
        private:                                                    \
            Type1 Param1;                                           \
            Type2 Param2;                                           \
            Type3 Param3;                                           \
            Type4 Param4;                                           \
            Type5 Param5;                                           \
            Type6 Param6;                                           \
            Type7 Param7;                                           \
            Type8 Param8;                                           \
        };                                                          \
        WRITE_MESSAGE(queue, MessageName,                           \
                      (Value1,                                      \
                       Value2,                                      \
                       Value3,                                      \
                       Value4,                                      \
                       Value5,                                      \
                       Value6,                                      \
                       Value7,                                      \
                       Value8))                                     \
    }

#define ENQUEUE_MESSAGE_9(queue, MessageName,                       \
                          Param1, Value1,                           \
                          Param2, Value2,                           \
                          Param3, Value3,                           \
                          Param4, Value4,                           \
                          Param5, Value5,                           \
                          Param6, Value6,                           \
                          Param7, Value7,                           \
                          Param8, Value8,                           \
                          Param9, Value9,                           \
                          Code)                                     \
    {                                                               \
                                                                    \
        using Type1 = std::decay<decltype(Value1)>::type;           \
        using Type2 = std::decay<decltype(Value2)>::type;           \
        using Type3 = std::decay<decltype(Value3)>::type;           \
        using Type4 = std::decay<decltype(Value4)>::type;           \
        using Type5 = std::decay<decltype(Value5)>::type;           \
        using Type6 = std::decay<decltype(Value6)>::type;           \
        using Type7 = std::decay<decltype(Value7)>::type;           \
        using Type8 = std::decay<decltype(Value8)>::type;           \
        using Type9 = std::decay<decltype(Value9)>::type;           \
                                                                    \
        class MessageName : public Message {                        \
        public:                                                     \
            MessageName(Type1 const &In##Param1,                    \
                        Type2 const &In##Param2,                    \
                        Type3 const &In##Param3,                    \
                        Type4 const &In##Param4,                    \
                        Type5 const &In##Param5,                    \
                        Type6 const &In##Param6,                    \
                        Type7 const &In##Param7,                    \
                        Type8 const &In##Param8,                    \
                        Type9 const &In##Param9)                    \
            : Param1(In##Param1),                                   \
              Param2(In##Param2),                                   \
              Param3(In##Param3),                                   \
              Param4(In##Param4),                                   \
              Param5(In##Param5),                                   \
              Param6(In##Param6),                                   \
              Param7(In##Param7),                                   \
              Param8(In##Param8),                                   \
              Param9(In##Param9) {                                  \
            }                                                       \
            virtual void execute() noexcept override {              \
                Code                                                \
            }                                                       \
            virtual char const *getName() const noexcept override { \
                return (#MessageName);                              \
            }                                                       \
                                                                    \
        private:                                                    \
            Type1 Param1;                                           \
            Type2 Param2;                                           \
            Type3 Param3;                                           \
            Type4 Param4;                                           \
            Type5 Param5;                                           \
            Type6 Param6;                                           \
            Type7 Param7;                                           \
            Type8 Param8;                                           \
            Type9 Param9;                                           \
        };                                                          \
        WRITE_MESSAGE(queue, MessageName,                           \
                      (Value1,                                      \
                       Value2,                                      \
                       Value3,                                      \
                       Value4,                                      \
                       Value5,                                      \
                       Value6,                                      \
                       Value7,                                      \
                       Value8,                                      \
                       Value9))                                     \
    }

} // namespace cc
