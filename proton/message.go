/*
Licensed to the Apache Software Foundation (ASF) under one
or more contributor license agreements.  See the NOTICE file
distributed with this work for additional information
regarding copyright ownership.  The ASF licenses this file
to you under the Apache License, Version 2.0 (the
"License"); you may not use this file except in compliance
with the License.  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing,
software distributed under the License is distributed on an
"AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
KIND, either express or implied.  See the License for the
specific language governing permissions and limitations
under the License.
*/

package proton

// #include <proton/types.h>
// #include <proton/message.h>
// #include <proton/codec.h>
import "C"

import (
	"qpid.apache.org/internal"
	"qpid.apache.org/amqp"
)

// HasMessage is true if all message data is available.
// Equivalent to !d.isNil && d.Readable() && !d.Partial()
func (d Delivery) HasMessage() bool { return !d.IsNil() && d.Readable() && !d.Partial() }

// Message decodes the message containined in a delivery.
//
// Must be called in the correct link context with this delivery as the current message,
// handling an MMessage event is always a safe context to call this function.
//
// Will return an error if message is incomplete or not current.
func (delivery Delivery) Message() (m amqp.Message, err error) {
	if !delivery.Readable() {
		return nil, internal.Errorf("delivery is not readable")
	}
	if delivery.Partial() {
		return nil, internal.Errorf("delivery has partial message")
	}
	data := make([]byte, delivery.Pending())
	result := delivery.Link().Recv(data)
	if result != len(data) {
		return nil, internal.Errorf("cannot receive message: %s", internal.PnErrorCode(result))
	}
	m = amqp.NewMessage()
	err = m.Decode(data)
	return
}

// TODO aconway 2015-04-08: proper handling of delivery tags. Tag counter per link.
var tags internal.IdCounter

// Send sends a amqp.Message over a Link.
// Returns a Delivery that can be use to determine the outcome of the message.
func (link Link) Send(m amqp.Message) (Delivery, error) {
	if !link.IsSender() {
		return Delivery{}, internal.Errorf("attempt to send message on receiving link")
	}
	delivery := link.Delivery(tags.Next())
	bytes, err := m.Encode(nil)
	if err != nil {
		return Delivery{}, internal.Errorf("cannot send mesage %s", err)
	}
	result := link.SendBytes(bytes)
	link.Advance()
	if result != len(bytes) {
		if result < 0 {
			return delivery, internal.Errorf("send failed %v", internal.PnErrorCode(result))
		} else {
			return delivery, internal.Errorf("send incomplete %v of %v", result, len(bytes))
		}
	}
	if link.RemoteSndSettleMode() == SndSettled {
		delivery.Settle()
	}
	return delivery, nil
}
