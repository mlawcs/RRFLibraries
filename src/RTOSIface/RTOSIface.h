/*
 * RTOS.h
 *
 *  Created on: 30 Mar 2018
 *      Author: David
 */

// This code makes the following assumptions about FreeRTOS:
// 1. The TaskHandle address returned by xTaskCreateStatic is the same as the 'storage' parameter that was passed to it.
// 2. The QueueHandle address returned by xSemaphoreCreateRecursiveMutexStatic is the same as the 'storage' parameter passed to it.
// 3. The QueueHandle address returned by xSemaphoreCreateBinaryStatic is the same as the 'storage' parameter passed to it.
//
// These assumptions allow us to derive our own task, mutex and semaphore classes from the FreeRTOS ones without storing the handle returned by those calls.

#ifndef SRC_RTOSIFACE_H_
#define SRC_RTOSIFACE_H_

#include "../ecv_duet3d.h"
#include <cstdint>
#include <utility>

#ifdef RTOS

# include "FreeRTOS.h"
# include "task.h"
# include "semphr.h"
# include "freertos_task_additions.h"
# include <atomic>

#endif

class TaskBase;						// when compiling without RTOS, this class is never defined and task handles are always nullptr
typedef TaskBase *TaskHandle;

/** \brief  Enable IRQ Interrupts

  This function enables IRQ interrupts by clearing the I-bit in the CPSR.
  Can only be executed in Privileged modes.
 */
__attribute__((always_inline)) static inline void EnableInterrupts() noexcept
{
	__asm volatile ("cpsie i" : : : "memory");
}

/** \brief  Disable IRQ Interrupts

  This function disables IRQ interrupts by setting the I-bit in the CPSR.
  Can only be executed in Privileged modes.
 */
__attribute__((always_inline)) static inline void DisableInterrupts() noexcept
{
	__asm volatile ("cpsid i" : : : "memory");
}

// Mutex class. This uses the FreeRTOS static semaphore type, but adds a name and links them all together in a list
class Mutex final
#ifdef RTOS
	: public StaticSemaphore_t
#endif
{
public:
	Mutex() noexcept
#ifdef RTOS
		: next(nullptr), name(nullptr)
#endif
	{ }

	void Create(const char *pName) noexcept;
	bool Take(uint32_t timeout = TimeoutUnlimited) noexcept;		// take ownership of the mutex returning true if successful, false if timed out
	bool Release() noexcept;
	TaskHandle GetHolder() const noexcept;

#ifdef RTOS
	const Mutex * null GetNext() const noexcept { return next; }
	const char * null GetName() const noexcept { return name; }
#endif

	Mutex(const Mutex&) = delete;
	Mutex& operator=(const Mutex&) = delete;

	static constexpr uint32_t TimeoutUnlimited = 0xFFFFFFFFu;

#ifdef RTOS
	static const Mutex * null GetMutexList() noexcept { return mutexList; }
#endif

private:

#ifdef RTOS
	Mutex * null next;
	const char * null name;
	QueueHandle_t GetHandle() noexcept { return reinterpret_cast<QueueHandle_t>(this); }

	// This next one is dangerous because of the const_cast. Use it only to call FreeRTOS functions that we know don't mutate the queue.
	QueueHandle_t GetConstHandle() const noexcept { return reinterpret_cast<const QueueHandle_t>(const_cast<Mutex*>(this)); }

	static Mutex * null mutexList;
#else
	void *handle;
#endif

};

class BinarySemaphore
#ifdef RTOS
	: public StaticSemaphore_t
#endif
{
public:
	BinarySemaphore() noexcept;

	bool Take(uint32_t timeout = TimeoutUnlimited) noexcept;
	bool Give() noexcept;

	static constexpr uint32_t TimeoutUnlimited = 0xFFFFFFFFu;

private:
#ifdef RTOS
	QueueHandle_t GetHandle() noexcept { return reinterpret_cast<QueueHandle_t>(this); }
#endif
};

#ifdef RTOS

// Our TaskBase structure now extends the FreeRTOS one
class TaskBase : public StaticTask_t
{
public:
	// Type of a short-form task ID. Task IDs start at 1 and each task takes the next available number.
	// If tasks are never deleted except at shutdown then we can guarantee that task IDs will be small number, because it won't exceed the number of tasks.
	// This is used by the CAN subsystem, so that we can use 8-bit task IDs to identify a sending task, instead of needing to use 32-bits.
	typedef uint32_t TaskId;

	TaskBase() noexcept : next(nullptr), taskId(0) { }
	~TaskBase() noexcept { TerminateAndUnlink(); }

	// Get the short-form task ID. This is a small number, used to send a task ID in 1 byte or less i a CAN packet. It is guaranteed not to be zero.
	TaskId GetTaskId() const noexcept { return taskId; }

	// Get the handle needed when making calls to FreeRTOS
	TaskHandle_t GetFreeRTOSHandle() noexcept { return reinterpret_cast<TaskHandle_t>(this); }

	// Functions to manage the task list
	// Link the task into the thread list and allocate a short task ID to it. This function is called directly for tasks that are created by FreeRTOS, so it must be public
	void AddToList() noexcept;
	void TerminateAndUnlink() noexcept;
	TaskBase *_ecv_from null GetNext() noexcept { return next; }

	void Suspend() noexcept { vTaskSuspend(GetFreeRTOSHandle()); }
	void Resume() noexcept { vTaskResume(GetFreeRTOSHandle()); }

	void SetPriority(unsigned int priority) noexcept { vTaskPrioritySet(GetFreeRTOSHandle(), priority); }

	static void SetCurrentTaskPriority(unsigned int priority) noexcept { vTaskPrioritySet(nullptr, priority); }

	bool IsRunning() const noexcept { return taskId != 0; }

	// Wake up a task identified by its handle from an ISR. Safe to call with a null handle.
	static void GiveFromISR(TaskBase *_ecv_from null h) noexcept
	{
		if (h != nullptr)				// check that the task exists
		{
			not_null(h)->GiveFromISR();
		}
	}

	// Wake up this task from an ISR
	void GiveFromISR() noexcept;

	// Wake up this task but not from an ISR
	void Give() noexcept
	{
		xTaskNotifyGive(GetFreeRTOSHandle());
	}

	// Wait until we have been woken up or we time out. Return true if successful, false if we timed out (same as for Mutex::Take()).
	static bool Take(uint32_t timeout = TimeoutUnlimited) noexcept
	{
		return ulTaskNotifyTake(pdTRUE, timeout) != 0;
	}

	// Clear a task notification count
	static uint32_t ClearNotifyCount(TaskBase *_ecv_from h, uint32_t bitsToClear = 0xFFFFFFFFu) noexcept
	{
		return ulTaskNotifyValueClear(h->GetFreeRTOSHandle(), bitsToClear);
	}

	// Clear the current task notification count
	static uint32_t ClearCurrentTaskNotifyCount(uint32_t bitsToClear = 0xFFFFFFFFu) noexcept
	{
		return ulTaskNotifyValueClear(nullptr, bitsToClear);
	}

	static TaskBase *_ecv_from GetCallerTaskHandle() noexcept { return reinterpret_cast<TaskBase *>(xTaskGetCurrentTaskHandle()); }

	static TaskId GetCallerTaskId() noexcept { return GetCallerTaskHandle()->taskId; }

	static void Yield() noexcept { taskYIELD(); }

	TaskBase(const TaskBase&) = delete;				// it's not safe to copy these
	TaskBase& operator=(const TaskBase&) = delete;	// it's not safe to assign these

	static TaskBase *_ecv_from null GetTaskList() noexcept { return taskList; }

	static const uint32_t *GetCurrentTaskStackBase() noexcept { return pxTaskGetCurrentStackBase(); }

	static constexpr uint32_t TimeoutUnlimited = 0xFFFFFFFFu;

protected:
	TaskBase *_ecv_from null next;
	TaskId taskId;

	static TaskBase *_ecv_from null taskList;
	static TaskId numTasks;
};

template<unsigned int StackWords> class Task : public TaskBase
{
public:
	// The Create function assumes that only the main task creates other tasks, so we don't need a mutex to protect the task list
	void Create(TaskFunction_t pxTaskCode, const char * pcName, void *pvParameters, unsigned int uxPriority) noexcept
	{
		xTaskCreateStatic(pxTaskCode, pcName, StackWords, pvParameters, uxPriority, stack, this);
		AddToList();
	}

	// These functions should be used only to tell FreeRTOS where the corresponding data is
	StaticTask_t *_ecv_from GetTaskMemory() noexcept { return this; }
	uint32_t *_ecv_array GetStackBase() noexcept { return stack; }
	uint32_t GetStackSize() const noexcept { return StackWords; }

private:
	uint32_t stack[StackWords];
};

#endif

// Class to lock a mutex and automatically release it when it goes out of scope
// If we pass a null mutex handle to the Locker constructor, it means there is nothing to lock and we pretend the lock has been acquired.
class MutexLocker
{
public:
	explicit MutexLocker(Mutex *null pm, uint32_t timeout = Mutex::TimeoutUnlimited) noexcept;	// acquire lock
	explicit MutexLocker(Mutex& pm, uint32_t timeout = Mutex::TimeoutUnlimited) noexcept;	// acquire lock
	~MutexLocker() { Release(); }

	void Release() noexcept;																// release the lock early (also gets released by destructor)
	bool ReAcquire(uint32_t timeout = Mutex::TimeoutUnlimited) noexcept;					// acquire it again, if it isn't already owned (non-counting)
	bool IsAcquired() const noexcept { return acquired; }

	MutexLocker(const MutexLocker&) = delete;
	MutexLocker& operator=(const MutexLocker&) = delete;
	MutexLocker(MutexLocker&& other) noexcept : handle(other.handle), acquired(other.acquired) { other.handle = nullptr; other.acquired = false; }

private:
	Mutex *null handle;
	bool acquired;
};

// Interface to RTOS or RTOS substitute
namespace RTOSIface
{
	inline TaskBase *GetCurrentTask() noexcept
	{
#ifdef RTOS
		return reinterpret_cast<TaskBase *>(xTaskGetCurrentTaskHandle());
#else
		return nullptr;
#endif
	}

#ifndef RTOS
	static volatile unsigned int interruptCriticalSectionNesting = 0;
#endif

	// Enter a critical section, where modification to variables by interrupts (and perhaps also other tasks) must be avoided
	inline void EnterInterruptCriticalSection() noexcept
	{
#ifdef RTOS
		taskENTER_CRITICAL();
#else
		DisableInterrupts();
		++interruptCriticalSectionNesting;
#endif
	}

	// Leave an interrupt-critical section
	inline void LeaveInterruptCriticalSection() noexcept
	{
#ifdef RTOS
		taskEXIT_CRITICAL();
#else
		--interruptCriticalSectionNesting;
		if (interruptCriticalSectionNesting == 0)
		{
			EnableInterrupts();
		}
#endif
	}

	// Enter a task-critical region. Used to protect concurrent access to variable from different tasks, where the variable are not used/modified by interrupts.
	// This can be called even if the caller is already in a TaskCriticalSection, because FreeRTOS keeps track of the nesting count.
	inline void EnterTaskCriticalSection() noexcept
	{
#ifdef RTOS
		vTaskSuspendAll();
#else
		// nothing to do here because there is no task preemption
#endif
	}

	// Exit a task-critical region, returning true if a task switch occurred
	inline bool LeaveTaskCriticalSection() noexcept
	{
#ifdef RTOS
		return xTaskResumeAll() == pdTRUE;
#else
		// nothing to do here because there is no task preemption
		return false;
#endif
	}

	inline void Yield() noexcept
	{
#ifdef RTOS
		taskYIELD();
#endif
	}
}

class InterruptCriticalSectionLocker
{
public:
	InterruptCriticalSectionLocker() noexcept { RTOSIface::EnterInterruptCriticalSection(); }
	~InterruptCriticalSectionLocker() noexcept { (void)RTOSIface::LeaveInterruptCriticalSection(); }

	InterruptCriticalSectionLocker(const InterruptCriticalSectionLocker&) = delete;
	InterruptCriticalSectionLocker(InterruptCriticalSectionLocker&&) = delete;
	InterruptCriticalSectionLocker& operator=(const InterruptCriticalSectionLocker&) = delete;
};

class TaskCriticalSectionLocker
{
public:
	TaskCriticalSectionLocker() noexcept { RTOSIface::EnterTaskCriticalSection(); }
	~TaskCriticalSectionLocker() noexcept { RTOSIface::LeaveTaskCriticalSection(); }

	TaskCriticalSectionLocker(const TaskCriticalSectionLocker&) = delete;
	TaskCriticalSectionLocker(TaskCriticalSectionLocker&&) = delete;
	TaskCriticalSectionLocker& operator=(const TaskCriticalSectionLocker&) = delete;
};

// Same as TaskCriticalSectionLocker except that we may not want to lock
class ConditionalTaskCriticalSectionLocker
{
public:
	explicit ConditionalTaskCriticalSectionLocker(bool doLock) noexcept : locked(doLock) { if (locked) { RTOSIface::EnterTaskCriticalSection(); } }
	~ConditionalTaskCriticalSectionLocker() noexcept { if (locked) { RTOSIface::LeaveTaskCriticalSection(); } }

	ConditionalTaskCriticalSectionLocker(const ConditionalTaskCriticalSectionLocker&) = delete;
	ConditionalTaskCriticalSectionLocker(ConditionalTaskCriticalSectionLocker&&) = delete;
	ConditionalTaskCriticalSectionLocker& operator=(const ConditionalTaskCriticalSectionLocker&) = delete;

private:
	bool locked;
};

// Class to represent a lock that allows multiple readers but only one writer
// This is designed to be efficient when writing is rare
// Rules:
// - Read locks are recursive. You can request a read lock on an object multiple times, but you must release it the same number of times.
// - Write locks are not recursive.
// - If you have a write lock on an object, you can request a read lock on the same object and it will be granted automatically.
// - If you have a read lock, you must not ask for a write lock on the same object, it will deadlock if you do.
// - You can downgrade a write lock to a read lock
// - There is no facility to upgrade a read lock to a write lock, because the system would deadlock if two tasks tried to do that at the same time
// - Except where noted, the member functions are not safe to call from an ISR.
class ReadWriteLock
{
public:
	ReadWriteLock() noexcept
#ifdef RTOS
		: readLocks(nullptr), writeLocks(nullptr)
#endif
	{ }

	void LockForReading() noexcept;
	bool ConditionalLockForReading() noexcept;
	void ReleaseReader() noexcept;
	void LockForWriting() noexcept;
	bool ConditionalLockForWriting() noexcept;
	void ReleaseWriter() noexcept;
	void DowngradeWriter() noexcept;					// turn a write lock into a read lock (but you can't go back again)
	bool IsWriteLocked() const noexcept;				// return true if there is an active write lock. Must only be called with interrupts disabled or scheduling disabled. Safe to call from an ISR.
#ifdef RTOS
	void CheckHasWriteLock() noexcept;
	void CheckHasReadLock() noexcept;
	void CheckHasReadOrWriteLock() noexcept;
#endif

	ReadWriteLock(const ReadWriteLock&) = delete;
	ReadWriteLock(const ReadWriteLock&&) = delete;
	ReadWriteLock& operator=(const ReadWriteLock&) = delete;

private:

#ifdef RTOS
	struct LockRecord;
	LockRecord *_ecv_null volatile readLocks;			// linked list of tasks that own or want to own a read lock
	LockRecord *_ecv_null volatile writeLocks;			// linked list of tasks that own or want to own a write lock
#endif
};

#ifndef RTOS

// Dummy lock functions
inline void ReadWriteLock::LockForReading() noexcept { }
inline bool ReadWriteLock::ConditionalLockForReading() noexcept { return true; }
inline void ReadWriteLock::ReleaseReader() noexcept { }
inline void ReadWriteLock::LockForWriting() noexcept { }
inline bool ReadWriteLock::ConditionalLockForWriting() noexcept { return true; }
inline void ReadWriteLock::ReleaseWriter() noexcept { }
inline void ReadWriteLock::DowngradeWriter() noexcept { }

#endif

class ReadLocker
{
public:
	explicit ReadLocker(ReadWriteLock& p_lock) noexcept : lock(&p_lock) { not_null(lock)->LockForReading(); }
	explicit ReadLocker(ReadWriteLock * null p_lock) noexcept : lock(p_lock) { if (lock != nullptr) { not_null(lock)->LockForReading(); } }
	ReadLocker(ReadLocker&& other) noexcept : lock(other.lock) { other.lock = nullptr; }
	~ReadLocker() { if (lock != nullptr) { not_null(lock)->ReleaseReader(); } }
	void Release() noexcept { if (lock != nullptr) { not_null(lock)->ReleaseReader(); lock = nullptr; } }

	ReadLocker(const ReadLocker&) = delete;
	ReadLocker& operator=(const ReadLocker&) = delete;

private:
	ReadWriteLock* null lock;
};

class ConditionalReadLocker
{
public:
	explicit ConditionalReadLocker(ReadWriteLock& p_lock) noexcept : lock((p_lock.ConditionalLockForReading()) ? &p_lock : nullptr) { }
	ConditionalReadLocker(ConditionalReadLocker&& other) noexcept : lock(other.lock) { other.lock = nullptr; }
	~ConditionalReadLocker() { if (lock != nullptr) { not_null(lock)->ReleaseReader(); } }
	bool IsLocked() const noexcept { return lock != nullptr; }

	ConditionalReadLocker(const ConditionalReadLocker&) = delete;
	ConditionalReadLocker& operator=(const ConditionalReadLocker&) = delete;

private:
	ReadWriteLock* null lock;
};

class WriteLocker
{
public:
	explicit WriteLocker(ReadWriteLock& p_lock) noexcept : lock(&p_lock) { not_null(lock)->LockForWriting(); }
	explicit WriteLocker(ReadWriteLock * null p_lock) noexcept : lock(p_lock) { if (lock != nullptr) { not_null(lock)->LockForWriting(); } }
	WriteLocker(WriteLocker&& other) noexcept : lock(other.lock) { other.lock = nullptr; }
	~WriteLocker() { if (lock != nullptr) { not_null(lock)->ReleaseWriter(); } }
	void Release() noexcept { if (lock != nullptr) { not_null(lock)->ReleaseWriter(); lock = nullptr; } }
	void Downgrade() noexcept { if (lock != nullptr) { not_null(lock)->DowngradeWriter(); } }

	WriteLocker(const WriteLocker&) = delete;
	WriteLocker& operator=(const WriteLocker&) = delete;

private:
	ReadWriteLock* null lock;
};

class ConditionalWriteLocker
{
public:
	explicit ConditionalWriteLocker(ReadWriteLock& p_lock) noexcept : lock((p_lock.ConditionalLockForWriting()) ? &p_lock : nullptr) { }
	ConditionalWriteLocker(ConditionalWriteLocker&& other) noexcept : lock(other.lock) { other.lock = nullptr; }
	~ConditionalWriteLocker() { if (lock != nullptr) { not_null(lock)->ReleaseWriter(); } }
	bool IsLocked() const noexcept { return lock != nullptr; }

	ConditionalWriteLocker(const ConditionalWriteLocker&) = delete;
	ConditionalWriteLocker& operator=(const ConditionalWriteLocker&) = delete;

private:
	ReadWriteLock* null lock;
};

template<class T> class ReadLockedPointer
{
public:
	ReadLockedPointer(ReadWriteLock& p_lock, T* p_ptr) noexcept : locker(p_lock), ptr(p_ptr) { }
	ReadLockedPointer(ReadLocker& p_locker, T* p_ptr) noexcept : locker(std::move(p_locker)), ptr(p_ptr) { }
	ReadLockedPointer(std::nullptr_t, T* p_ptr) noexcept : locker(nullptr), ptr(p_ptr) { }
	ReadLockedPointer(ReadLockedPointer<T>&& other) noexcept : locker(std::move(other.locker)), ptr(other.ptr) { other.ptr = nullptr; }

	bool IsNull() const noexcept { return ptr == nullptr; }
	bool IsNotNull() const noexcept { return ptr != nullptr; }
	T* operator->() const noexcept { return ptr; }
	T& operator*() const noexcept { return *ptr; }
	T* Ptr() const noexcept { return ptr; }

	void Release() noexcept { ptr = nullptr; locker.Release(); }

	ReadLockedPointer(const ReadLockedPointer<T>&) = delete;
	ReadLockedPointer<T>& operator=(const ReadLockedPointer<T>&) = delete;

private:
	ReadLocker locker;
	T* null ptr;
};

template<class T> class WriteLockedPointer
{
public:
	WriteLockedPointer(ReadWriteLock& p_lock, T* p_ptr) noexcept : locker(p_lock), ptr(p_ptr) { }
	WriteLockedPointer(WriteLocker& p_locker, T* null p_ptr) noexcept : locker(std::move(p_locker)), ptr(p_ptr) { }
	WriteLockedPointer(std::nullptr_t, T* null p_ptr) noexcept : locker(nullptr), ptr(p_ptr) { }
	WriteLockedPointer(WriteLockedPointer<T>&& other) noexcept : locker(std::move(other.locker)), ptr(other.ptr) { other.ptr = nullptr; }

	bool IsNull() const noexcept { return ptr == nullptr; }
	bool IsNotNull() const noexcept { return ptr != nullptr; }
	T* operator->() const noexcept { return ptr; }
	T& operator*() const noexcept { return *ptr; }
	T* Ptr() const noexcept { return ptr; }

	WriteLockedPointer(const WriteLockedPointer<T>&) = delete;
	WriteLockedPointer<T>& operator=(const WriteLockedPointer<T>&) = delete;

private:
	WriteLocker locker;
	T* null ptr;
};

#ifdef RTOS

// Queue support
class QueueBase
{
public:
	QueueBase() noexcept : handle(nullptr), next(nullptr), name(nullptr) { }

	const QueueBase * null GetNext() const noexcept { return next; }

	static const QueueBase * null GetThread() noexcept { return thread; }

protected:
	QueueHandle_t null handle;
	QueueBase * null next;
	const char * null name;
	StaticQueue_t storage;

	static QueueBase * null thread;
};

template <class Message> class MessageQueue : public QueueBase
{
public:
	MessageQueue() noexcept : messageStorage(nullptr) { }

	void Create(const char *p_name, size_t capacity) noexcept;
	bool PutToBack(const Message &m, uint32_t timeout) noexcept;
	bool PutToFront(const Message &m, uint32_t timeout) noexcept;
	bool Get(Message& m, uint32_t timeout) noexcept;
	bool IsValid() const noexcept { return handle != nullptr; }

private:
	uint8_t * _ecv_array null messageStorage;
};

template <class Message> void MessageQueue<Message>::Create(const char *p_name, size_t capacity) noexcept
{
	if (handle == nullptr)
	{
		messageStorage = new uint8_t[capacity * sizeof(Message)];
		handle = xQueueCreateStatic(capacity, sizeof(Message), messageStorage, &storage);
	}
}

template <class Message> bool MessageQueue<Message>::PutToBack(const Message &m, uint32_t timeout) noexcept
{
	return xQueueSendToBack(handle, &m, timeout) == pdTRUE;
}

template <class Message> bool MessageQueue<Message>::PutToFront(const Message &m, uint32_t timeout) noexcept
{
	return xQueueSendToFront(handle, &m, timeout) == pdTRUE;
}

template <class Message> bool MessageQueue<Message>::Get(Message& m, uint32_t timeout) noexcept
{
	return xQueueReceive(handle, &m, timeout) == pdTRUE;
}

#endif

#endif /* SRC_RTOSIFACE_H_ */
