/*
 * buffers.hpp
 *
 *  Created on: Dec 14, 2015
 *      Author: zmij
 */

#ifndef TIP_WIRE_BUFFERS_HPP_
#define TIP_WIRE_BUFFERS_HPP_

#include <wire/asio_config.hpp>
#include <wire/encoding/wire_io.hpp>
#include <wire/encoding/message.hpp>
#include <wire/core/connector_fwd.hpp>
#include <wire/encoding/detail/buffer_sequence.hpp>

#include <memory>
#include <iterator>
#include <vector>

namespace wire {
namespace encoding {

class incoming;

class outgoing {
public:
    /** Internal buffers storage type */
    using buffer_type               = detail::buffer_sequence::buffer_type;
    /** Sequence of internal buffers */
    using buffer_sequence_type      = detail::buffer_sequence::buffer_sequence_type;
    using const_buffer              = asio_ns::const_buffer;
    using asio_buffers              = ::std::vector< const_buffer >;
    using asio_shared_buffers       = ::std::shared_ptr<asio_buffers>;
    //@{
    /**
     * @name Container concept
     * http://en.cppreference.com/w/cpp/concept/Container
     */
    using value_type                = detail::buffer_sequence::value_type;
    using reference                 = detail::buffer_sequence::reference;
    using const_reference           = detail::buffer_sequence::const_reference;
    using pointer                   = detail::buffer_sequence::pointer;
    using const_pointer             = detail::buffer_sequence::const_pointer;
    using difference_type           = detail::buffer_sequence::difference_type;
    using size_type                 = detail::buffer_sequence::size_type;
    //@}
public:
    /** Normal input iterator for output buffer */
    using iterator                  = detail::buffer_sequence::iterator;
    /** Constant input iterator for output buffer */
    using const_iterator            = detail::buffer_sequence::const_iterator;
    /** Reverse input iterator */
    using reverse_iterator          = detail::buffer_sequence::reverse_iterator;
    /** Constant reverse input iterator */
    using const_reverse_iterator    = detail::buffer_sequence::const_reverse_iterator;

    using encapsulation_type        = detail::buffer_sequence::out_encaps;
    using encaps_guard              = detail::encaps_guard<encapsulation_type>;

    using output_iterator           = ::std::back_insert_iterator<outgoing>;
public:
    outgoing(core::connector_ptr cnctr);
    outgoing(core::connector_ptr cnctr, message::message_flags);
    /** Copy construct */
    outgoing(outgoing const&) = delete;
    /** Move construct */
    outgoing(outgoing&&);

    ~outgoing();

    /** Swap contents */
    void
    swap(outgoing&);

    /** Copy assign */
    outgoing&
    operator = (outgoing const&) = delete;
    /** Move assign */
    outgoing&
    operator = (outgoing&&);

    core::connector_ptr
    get_connector() const;

    message::message_flags
    type() const;
    //@{
    /**
     * @name Container concept
     * http://en.cppreference.com/w/cpp/concept/Container
     */
    size_type
    size() const;
    bool
    empty() const;

    iterator
    begin();
    inline const_iterator
    begin() const
    { return cbegin(); }
    const_iterator
    cbegin() const;

    iterator
    end();
    inline const_iterator
    end() const
    { return cend(); }

    const_iterator
    cend() const;
    //@}

    //@{
    /** @name ReversibleContainer concept */
    reverse_iterator
    rbegin();
    inline const_reverse_iterator
    rbegin() const
    { return crbegin(); }
    const_reverse_iterator
    crbegin() const;

    reverse_iterator
    rend();
    inline const_reverse_iterator
    rend() const
    { return crend(); }
    const_reverse_iterator
    crend() const;
    //@}

    //@{
    /**
     * @name SequenceContainer concept
     * http://en.cppreference.com/w/cpp/concept/SequenceContainer
     */
    reference
    front();
    const_reference
    front() const;

    reference
    back();
    const_reference
    back() const;

    void
    push_back(value_type);
    void
    pop_back();

    reference
    operator[] (size_type);
    const_reference
    operator[] (size_type) const;

    reference
    at(size_type);
    const_reference
    at(size_type) const;
    //@}

    /**
     * Get asio buffers for output
     * @return
     */
    asio_shared_buffers
    to_buffers(bool merge_buffers = false) const;

    //@{
    /** @name Encapsulated data */
    void
    insert_encapsulation(outgoing&&);

    encapsulation_type
    begin_encapsulation();

    encapsulation_type
    current_encapsulation();

    void
    close_all_encaps();
    //@}

    //@{
    void
    debug_print(::std::ostream&) const;
    //@}
private:
    friend class incoming;
    struct impl;
    using pimpl = ::std::unique_ptr<impl>;
    pimpl pimpl_;
};

using outgoing_ptr = ::std::shared_ptr< outgoing >;

inline void
swap(outgoing& lhs, outgoing& rhs)
{
    lhs.swap(rhs);
}

class incoming {
public:
    /** Internal buffers storage type */
    using buffer_type               = ::std::vector<uint8_t>;
    /** Sequence of internal buffers */
    using buffer_sequence_type      = ::std::vector<buffer_type>;
    //@{
    /**
     * @name Container concept
     * http://en.cppreference.com/w/cpp/concept/Container
     */
    using value_type                = buffer_type::value_type;
    using reference                 = buffer_type::reference;
    using const_reference           = buffer_type::const_reference;
    using pointer                   = buffer_type::pointer;
    using const_pointer             = buffer_type::const_pointer;
    using difference_type           = buffer_type::difference_type;
    using size_type                 = buffer_type::size_type;
    //@}
public:
    /** Normal input iterator for input buffer */
    using iterator                  = detail::buffer_sequence::iterator;
    /** Constant input iterator for input buffer */
    using const_iterator            = detail::buffer_sequence::const_iterator;
    /** Reverse input iterator */
    using reverse_iterator          = detail::buffer_sequence::reverse_iterator;
    /** Constant reverse input iterator */
    using const_reverse_iterator    = detail::buffer_sequence::const_reverse_iterator;

    using encapsulation_type        = detail::buffer_sequence::in_encaps;
    using encaps_guard              = detail::encaps_guard<encapsulation_type>;
public:
    /**
     * Construct incoming buffer sequence with a message header
     * @param
     */
    incoming(core::connector_ptr cnctr, message const&);
    /**
     * Construct incoming buffer sequence and copy buffers
     * @param
     * @param begin
     * @param end
     */
    template < typename InputIterator >
    incoming(core::connector_ptr cnctr, message const&, InputIterator& begin, InputIterator end);
    /**
     * Construct incoming buffer sequence and copy a buffer
     * @param
     * @param
     */
    incoming(core::connector_ptr cnctr, message const&, buffer_type const&);
    /**
     * Construct incoming buffer and move a buffer
     * @param
     * @param
     */
    incoming(core::connector_ptr cnctr, message const&, buffer_type&&);
    /**
     * Construct incoming buffer from an outgoing buffer
     * @param
     * @param
     */
    incoming(message const&, outgoing&&);

    core::connector_ptr
    get_connector() const;

    message const&
    header() const;
    message::message_flags
    type() const;

    template < typename InputIterator >
    void
    insert_back(InputIterator& begin, InputIterator end);
    void
    insert_back(buffer_type const&);
    void
    insert_back(buffer_type&&);

    /**
     * Check if the message has been read from network buffers.
     * @return
     */
    bool
    complete() const;
    /**
     * How many bytes is left to read from network buffers
     * @return
     */
    size_type
    want_bytes() const;

    //@{
    /**
     * @name Container concept
     * http://en.cppreference.com/w/cpp/concept/Container
     */
    size_type
    size() const;
    bool
    empty() const;

    iterator
    begin();
    inline const_iterator
    begin() const
    { return cbegin(); }
    const_iterator
    cbegin() const;

    iterator
    end();
    inline const_iterator
    end() const
    { return cend(); }

    const_iterator
    cend() const;
    //@}

    //@{
    /** @name ReversibleContainer concept */
    reverse_iterator
    rbegin();
    inline const_reverse_iterator
    rbegin() const
    { return crbegin(); }
    const_reverse_iterator
    crbegin() const;

    reverse_iterator
    rend();
    inline const_reverse_iterator
    rend() const
    { return crend(); }
    const_reverse_iterator
    crend() const;
    //@}

    //@{
    /** @name Encapsulated data */
    encapsulation_type
    begin_encapsulation(const_iterator);

    encapsulation_type
    current_encapsulation();
    //@}
    //@{
    void
    debug_print(::std::ostream&) const;
    //@}
private:
    void
    create_pimpl(core::connector_ptr cnctr, message const&);

    buffer_type&
    back_buffer();
private:
    struct impl;
    using pimpl = ::std::shared_ptr<impl>;
    pimpl pimpl_;
};

using incoming_ptr = ::std::shared_ptr<incoming>;

// TODO Chage the signature to an encapsulation
using reply_callback
        = ::std::function< void(incoming::const_iterator, incoming::const_iterator) >;
using request_result_callback = ::std::function< void(outgoing&&) >;

}  // namespace encoding
}  // namespace wire

#include <wire/encoding/buffers.inl>

#endif /* TIP_WIRE_BUFFERS_HPP_ */
