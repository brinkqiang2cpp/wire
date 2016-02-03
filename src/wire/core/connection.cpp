/*
 * connection.cpp
 *
 *  Created on: Jan 28, 2016
 *      Author: zmij
 */

#include <wire/core/connection.hpp>
#include <wire/core/detail/connection_impl.hpp>
#include <wire/encoding/message.hpp>

#include <iterator>

namespace wire {
namespace core {

namespace detail {

connection_impl_ptr
connection_impl_base::create_connection(asio_config::io_service_ptr io_svc,
		transport_type _type)
{
	switch (_type) {
		case transport_type::tcp :
			return ::std::make_shared<
					connection_impl< transport_type::tcp > >( io_svc );
		default:
			break;
	}
	throw errors::logic_error(_type, " connection is not implemented yet");
}

void
connection_impl_base::connect_async(endpoint const& ep,
		callbacks::void_callback cb, callbacks::exception_callback eb)
{
	mode_ = client;
	process_event(events::connect{ ep, cb, eb });
}

void
connection_impl_base::handle_connected(asio_config::error_code const& ec)
{
	if (!ec) {
		process_event(events::connected{});
	} else {
		process_event(events::connection_failure{
			::std::make_exception_ptr(errors::connection_failed(ec.message()))
		});
	}
}

void
connection_impl_base::send_validate_message()
{
	encoding::outgoing_ptr out =
		::std::make_shared<encoding::outgoing>(
				encoding::message::validate_flags);
	write_async(out);
}


void
connection_impl_base::close()
{
	process_event(events::close{});
}

void
connection_impl_base::send_close_message()
{
	encoding::outgoing_ptr out = ::std::make_shared<encoding::outgoing>(encoding::message::close);
	write_async(out);
}

void
connection_impl_base::write_async(encoding::outgoing_ptr out,
		callbacks::void_callback cb)
{
	std::cerr << "Send message " << out->type() << " size " << out->size() << "\n";
	do_write_async( out,
		std::bind(&connection_impl_base::handle_write, shared_from_this(),
				std::placeholders::_1, std::placeholders::_2, cb, out));
}

void
connection_impl_base::handle_write(asio_config::error_code const& ec, std::size_t bytes,
		callbacks::void_callback cb, encoding::outgoing_ptr out)
{
	if (!ec) {
		if (cb) cb();
	} else {
		process_event(events::connection_failure{
			::std::make_exception_ptr(errors::connection_failed(ec.message()))
		});
	}
}

void
connection_impl_base::start_read()
{
	incoming_buffer_ptr buffer = ::std::make_shared< incoming_buffer >();
	read_async(buffer);
}

void
connection_impl_base::read_async(incoming_buffer_ptr buffer)
{
	do_read_async(buffer,
		std::bind(&connection_impl_base::handle_read, shared_from_this(),
				std::placeholders::_1, std::placeholders::_2, buffer));
}

void
connection_impl_base::handle_read(asio_config::error_code const& ec, std::size_t bytes,
		incoming_buffer_ptr buffer)
{
	if (!ec) {
		read_incoming_message(buffer, bytes);
		start_read();
	} else {
		process_event(events::connection_failure{
			::std::make_exception_ptr(errors::connection_failed(ec.message()))
		});
	}
}

void
connection_impl_base::read_incoming_message(incoming_buffer_ptr buffer, std::size_t bytes)
{
	using encoding::message;
	auto b = buffer->begin();
	auto e = b + bytes;
	try {
		while (b != e) {
			if (incoming_) {
				incoming_->insert_back(b, e);
				if (incoming_->complete()) {
					dispatch_incoming(incoming_);
					incoming_.reset();
				}
			} else {
				message m;
				read(b, e, m);
				bytes -= b - buffer->begin();

				switch(m.type()) {
					case message::validate: {
						if (m.size > 0) {
							throw errors::connection_failed("Invalid validate message");
						}
						process_event(events::receive_validate{});
						break;
					}
					case message::close : {
						if (m.size > 0) {
							throw errors::connection_failed("Invalid close message");
						}
						process_event(events::receive_close{});
						break;
					}
					default: {
						if (m.size == 0) {
							throw errors::connection_failed(
									"Zero sized ", m.type(), " message");
						}
						std::cerr << "Receive message " << m.type()
								<< " size " << m.size << "\n";

						encoding::incoming_ptr incoming =
							std::make_shared< encoding::incoming >( m, b, e );
						if (!incoming->complete()) {
							incoming_ = incoming;
						} else {
							dispatch_incoming(incoming);
						}
					}
				}
			}
		}
	} catch (std::exception const& e) {
		/** TODO Make it a protocol error? Can we handle it? */
		std::cerr << "Protocol read exception: " << e.what() << "\n";
		process_event(events::connection_failure{
			::std::current_exception()
		});
	}
}

void
connection_impl_base::dispatch_incoming(encoding::incoming_ptr incoming)
{
	using encoding::message;
	switch (incoming->type()) {
		case message::request:
			process_event(events::receive_request{ incoming });
			break;
		case message::reply:
			process_event(events::receive_reply{ incoming });
			break;
		default:
			process_event(events::connection_failure{
				std::make_exception_ptr(errors::unmarshal_error{ "Unknown message type ", incoming->type() })
			});
	}
}

void
connection_impl_base::invoke_async(identity const& id, std::string const& op,
		encoding::outgoing&& params,
		encoding::reply_callback reply,
		callbacks::exception_callback exception,
		callbacks::callback< bool > sent)
{
	using encoding::request;
	encoding::outgoing_ptr out = std::make_shared<encoding::outgoing>(encoding::message::request);
	request r{
		++request_no_,
		encoding::operation_specs{ id, "", op },
		request::normal
	};
	write(std::back_inserter(*out), r);
	out->insert_encapsulation(std::move(params));
	callbacks::void_callback write_cb = sent ? [sent](){sent(true);} : callbacks::void_callback{};
	pending_replies_.insert(std::make_pair( r.number, pending_reply{ reply, exception } ));
	process_event(events::send_request{ out, write_cb });
}

void
connection_impl_base::dispatch_reply(encoding::incoming_ptr buffer)
{
	using namespace encoding;
	try {
		reply rep;
		incoming::const_iterator b = buffer->begin();
		incoming::const_iterator e = buffer->end();
		read(b, e, rep);
		auto f = pending_replies_.find(rep.number);
		if (f != pending_replies_.end()) {
			version ever;
			read(b, e, ever);
			uint64_t esize;
			read(b, e, esize);
			std::cerr << "Reply encaps v" << ever.major << "." << ever.minor
					<< " size " << esize << "\n";
			switch (rep.status) {
				case reply::success: {
					if (f->second.reply) {
						try {
							f->second.reply(b, e);
						} catch (...) {

						}
					}
					break;
				}
				default:
					if (f->second.error) {
						try {
							f->second.error(std::make_exception_ptr( errors::runtime_error("Wire exception") ));
						} catch (...) {

						}
					}
					break;
			}
		} // else discard the reply (it can be timed out)
	} catch (...) {
		process_event(events::connection_failure{ std::current_exception() });
	}
}

void
connection_impl_base::dispatch_request(encoding::incoming_ptr buffer)
{
	using namespace encoding;
	try {
		request req;
		incoming::const_iterator b = buffer->begin();
		incoming::const_iterator e = buffer->end();
		read(b, e, req);
		//Find invocation by req.operation.identity
	} catch (...) {
		process_event(events::connection_failure{ std::current_exception() });
	}
}

}  // namespace detail

struct connection::impl {
	asio_config::io_service_ptr	io_service_;
	detail::connection_impl_ptr connection_;

	impl(asio_config::io_service_ptr io_service)
		: io_service_(io_service)
	{
	}
	void
	connect_async(endpoint const& ep,
			callbacks::void_callback cb, callbacks::exception_callback eb)
	{
		if (connection_) {
			// Do something with the old connection
			// Or throw exception
		}
		connection_ = detail::connection_impl_base::create_connection(io_service_, ep.transport());
		connection_->connect_async(ep, cb, eb);
	}

	void
	close()
	{
		if (connection_) {
			connection_->close();
		} /** @todo Throw exception */
	}

	void
	invoke_async(identity const& id, std::string const& op,
			encoding::outgoing&& params,
			encoding::reply_callback reply,
			callbacks::exception_callback exception,
			callbacks::callback< bool > sent)
	{
		if (connection_) {
			connection_->invoke_async(id, op, std::move(params), reply, exception, sent);
		} /** @todo Throw exception */
	}
};

connection::connection(asio_config::io_service_ptr io_svc)
	: pimpl_(::std::make_shared<impl>(io_svc))
{
}

connection::connection(asio_config::io_service_ptr io_svc, endpoint const& ep,
		callbacks::void_callback cb, callbacks::exception_callback eb)
	: pimpl_(::std::make_shared<impl>(io_svc))
{
	pimpl_->connect_async(ep, cb, eb);
}

void
connection::connect_async(endpoint const& ep,
		callbacks::void_callback cb, callbacks::exception_callback eb)
{
	pimpl_->connect_async(ep, cb, eb);
}

void
connection::close()
{
	pimpl_->close();
}

void
connection::invoke_async(identity const& id, std::string const& op,
		encoding::outgoing&& params,
		encoding::reply_callback reply,
		callbacks::exception_callback exception,
		callbacks::callback< bool > sent)
{
	pimpl_->invoke_async(id, op, std::move(params), reply, exception, sent);
}


}  // namespace core
}  // namespace wire
