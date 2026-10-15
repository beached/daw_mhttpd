// Coroutine support for daw_mhttpd.
//
// task<T> is lazy and has no executor: it starts when awaited and, when it
// finishes, its awaiter continues on whichever thread finished it. Use
// from_callback to await callback based APIs; the coroutine continues on the
// thread that invokes the completion.
#pragma once

#include <daw/daw_ensure.h>

#include <atomic>
#include <coroutine>
#include <exception>
#include <functional>
#include <future>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace daw::mhttpd {
	template<typename T = void>
	class task;
}

namespace daw::mhttpd::detail {
	struct promise_base {
		std::coroutine_handle<> continuation;
		std::exception_ptr exception;
		// Set by whichever of the awaiter or the final suspend gets there
		// first; the second one continues the awaiter
		std::atomic<bool> ready{ false };

		[[nodiscard]] std::suspend_always initial_suspend( ) const noexcept {
			return { };
		}

		void unhandled_exception( ) noexcept {
			exception = std::current_exception( );
		}
	};

	// When the task finished synchronously the awaiter continues without
	// being resumed from here, so loops of synchronously completing awaits do
	// not grow the stack. This does not depend on the compiler turning
	// symmetric transfer into a tail call, which GCC does not do at -O0.
	template<typename Promise>
	struct final_awaiter {
		[[nodiscard]] bool await_ready( ) const noexcept {
			return false;
		}

		void await_suspend( std::coroutine_handle<Promise> h ) const noexcept {
			auto &p = h.promise( );
			if( p.ready.exchange( true, std::memory_order_acq_rel ) ) {
				p.continuation.resume( );
			}
		}

		void await_resume( ) const noexcept {}
	};

	template<typename T>
	struct task_promise : promise_base {
		std::optional<T> value;

		task<T> get_return_object( ) noexcept;

		final_awaiter<task_promise> final_suspend( ) const noexcept {
			return { };
		}

		template<typename U = T>
		requires std::constructible_from<T, U &&>
		void return_value( U &&v ) {
			value.emplace( std::forward<U>( v ) );
		}

		T result( ) {
			if( exception ) {
				std::rethrow_exception( exception );
			}
			return std::move( *value );
		}
	};

	template<>
	struct task_promise<void> : promise_base {
		task<void> get_return_object( ) noexcept;

		[[nodiscard]] final_awaiter<task_promise> final_suspend( ) const noexcept {
			return { };
		}

		void return_void( ) const noexcept {}

		void result( ) {
			if( exception ) {
				std::rethrow_exception( exception );
			}
		}
	};

	// Eagerly started, self destroying coroutine used as the root of a chain
	struct detached {
		struct promise_type {
			[[nodiscard]] detached get_return_object( ) const noexcept {
				return { };
			}

			[[nodiscard]] std::suspend_never initial_suspend( ) const noexcept {
				return { };
			}

			[[nodiscard]] std::suspend_never final_suspend( ) const noexcept {
				return { };
			}

			void return_void( ) const noexcept {}

			[[noreturn]] void unhandled_exception( ) const noexcept {
				std::terminate( );
			}
		};
	};
} // namespace daw::mhttpd::detail

namespace daw::mhttpd {
	template<typename T>
	class [[nodiscard]] task {
		static_assert( not std::is_reference_v<T>,
		               "Task<T&> is not supported, use a pointer" );

	public:
		using promise_type = detail::task_promise<T>;

		task( task &&other ) noexcept
		  : m_handle{ std::exchange( other.m_handle, { } ) } {}

		task &operator=( task &&other ) noexcept {
			if( this != &other ) {
				destroy( );
				m_handle = std::exchange( other.m_handle, { } );
			}
			return *this;
		}

		task( task const & ) = delete;
		task &operator=( task const & ) = delete;

		~task( ) {
			destroy( );
		}

		// A task is awaited once: co_await std::move( task ) or a prvalue
		auto operator co_await( ) && noexcept {
			daw_dbg_ensure( m_handle ); // awaiting a moved-from task
			struct awaiter {
				std::coroutine_handle<promise_type> handle;

				[[nodiscard]] bool await_ready( ) const noexcept {
					return false;
				}

				[[nodiscard]] bool
				await_suspend( std::coroutine_handle<> caller ) const noexcept {
					auto &p = handle.promise( );
					p.continuation = caller;
					handle.resume( );
					// false: the task already finished, continue without suspending
					return not p.ready.exchange( true, std::memory_order_acq_rel );
				}

				T await_resume( ) const {
					return handle.promise( ).result( );
				}
			};
			return awaiter{ m_handle };
		}

	private:
		friend promise_type;

		explicit task( std::coroutine_handle<promise_type> h ) noexcept
		  : m_handle{ h } {}

		void destroy( ) noexcept {
			if( m_handle ) {
				m_handle.destroy( );
			}
		}

		std::coroutine_handle<promise_type> m_handle;
	};
} // namespace daw::mhttpd

namespace daw::mhttpd::detail {
	template<typename T>
	task<T> task_promise<T>::get_return_object( ) noexcept {
		return task<T>{
		  std::coroutine_handle<task_promise<T>>::from_promise( *this ) };
	}

	inline task<void> task_promise<void>::get_return_object( ) noexcept {
		return task<void>{
		  std::coroutine_handle<task_promise<void>>::from_promise( *this ) };
	}

	template<typename T>
	detached sync_wait_root( task<T> task, std::promise<T> result ) {
		try {
			if constexpr( std::is_void_v<T> ) {
				co_await std::move( task );
				result.set_value( );
			} else {
				result.set_value( co_await std::move( task ) );
			}
		} catch( ... ) { result.set_exception( std::current_exception( ) ); }
	}
} // namespace daw::mhttpd::detail

namespace daw::mhttpd {
	// Runs the task and blocks the calling thread until it completes
	template<typename T>
	T sync_wait( task<T> task ) {
		auto result = std::promise<T>{ };
		auto future = result.get_future( );
		detail::sync_wait_root( std::move( task ), std::move( result ) );
		return future.get( );
	}

	// ---------------------------------------------------------------------------
	// from_callback
	// ---------------------------------------------------------------------------

	// Thrown from co_await when a completion is destroyed without being invoked
	class callback_abandoned : public std::runtime_error {
	public:
		callback_abandoned( )
		  : std::runtime_error{ "daw::mhttpd: completion was never invoked" } {}
	};
} // namespace daw::mhttpd

namespace daw::mhttpd::detail {
	template<typename T>
	struct callback_state {
		std::coroutine_handle<> handle;
		// Set by whichever of await_suspend or the completion finishes first;
		// the second one resumes (or continues) the coroutine
		std::atomic<bool> ready{ false };
		std::optional<std::conditional_t<std::is_void_v<T>, bool, T>> value;
		std::exception_ptr exception;

		void signal( ) noexcept {
			if( ready.exchange( true, std::memory_order_acq_rel ) ) {
				handle.resume( );
			}
		}
	};
} // namespace daw::mhttpd::detail

namespace daw::mhttpd {
	// Move-only, single-shot callback handed to from_callback's start function.
	// Destroying it without invoking it resumes the coroutine with
	// callback_abandoned.
	template<typename T = void>
	class completion {
	public:
		completion( completion &&other ) noexcept
		  : m_state{ std::exchange( other.m_state, nullptr ) } {}
		completion &operator=( completion && ) = delete;
		completion( completion const & ) = delete;
		completion &operator=( completion const & ) = delete;

		~completion( ) {
			if( m_state ) {
				fail( std::make_exception_ptr( callback_abandoned{ } ) );
			}
		}

		void operator( )( )
		requires std::is_void_v<T>
		{
			if( auto *s = std::exchange( m_state, nullptr ) ) {
				s->value.emplace( true );
				s->signal( );
			}
		}

		template<typename U = T>
		requires( not std::is_void_v<T> and std::constructible_from<T, U &&> )
		void operator( )( U &&v ) {
			if( auto *s = std::exchange( m_state, nullptr ) ) {
				try {
					s->value.emplace( std::forward<U>( v ) );
				} catch( ... ) { s->exception = std::current_exception( ); }
				s->signal( );
			}
		}

		// Resumes the coroutine by throwing `e` from co_await
		void fail( std::exception_ptr e ) noexcept {
			if( auto *s = std::exchange( m_state, nullptr ) ) {
				s->exception = std::move( e );
				s->signal( );
			}
		}

	private:
		template<typename, typename>
		friend class callback_awaitable;

		explicit completion( detail::callback_state<T> *s ) noexcept
		  : m_state{ s } {}

		detail::callback_state<T> *m_state;
	};

	template<typename T, typename Start>
	class [[nodiscard]] callback_awaitable {
	public:
		explicit callback_awaitable( Start start )
		  : m_start{ std::move( start ) } {}

		[[nodiscard]] bool await_ready( ) const noexcept {
			return false;
		}

		// If start throws, it must not have handed the completion off
		bool await_suspend( std::coroutine_handle<> h ) {
			m_state.handle = h;
			std::invoke( std::move( m_start ), completion<T>{ &m_state } );
			// false: the completion already ran, continue without suspending
			return not m_state.ready.exchange( true, std::memory_order_acq_rel );
		}

		T await_resume( ) {
			if( m_state.exception ) {
				std::rethrow_exception( m_state.exception );
			}
			if constexpr( not std::is_void_v<T> ) {
				return std::move( *m_state.value );
			}
		}

	private:
		Start m_start;
		detail::callback_state<T> m_state;
	};

	// co_await from_callback<T>( []( completion<T> done ) { ... } ) starts the
	// operation and suspends until done( value ), done.fail( e ), or done is
	// destroyed
	template<typename T = void, typename Start>
	requires std::is_invocable_v<std::decay_t<Start>, completion<T>>
	[[nodiscard]] callback_awaitable<T, std::decay_t<Start>>
	from_callback( Start &&start ) {
		return callback_awaitable<T, std::decay_t<Start>>{
		  std::forward<Start>( start ) };
	}
} // namespace daw::mhttpd
