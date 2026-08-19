#pragma once

/**
 * @file HAPMessages.hpp
 * @brief Every HAP payload, as a typed structure.
 *
 * One include for code that dispatches on HAPFrame::message() and therefore
 * touches most of the catalogue. Code that only speaks one part of the protocol
 * should include that part instead - a reporter has no business compiling the
 * link messages.
 *
 * | Header | Covers |
 * | ------ | ------ |
 * | HAPBindMessages | BindAnnounce, BindAccept, BindConfirm, ChildAttached |
 * | HAPDataMessages | Describe, Report, Read, Write, SetPolicy, SetName |
 * | HAPControlMessages | Ping, Pong, Ack, Nack, RouteError |
 * | HAPLinkMessages | SetLink, ClearLink, ListLinks |
 *
 * Every one of them is a plain structure with encode() and decode(), and no
 * knowledge of frames, radios or state: a payload knows its own bytes and
 * nothing else. What surrounds it is HAPFrame's business, and when to send one
 * is HAPStack's.
 */

#include <HAPMessages/HAPBindMessages.hpp>
#include <HAPMessages/HAPControlMessages.hpp>
#include <HAPMessages/HAPDataMessages.hpp>
#include <HAPMessages/HAPLinkMessages.hpp>
#include <HAPMessages/HAPMessageParts.hpp>
