/*
 * CDE - Common Desktop Environment
 *
 * Copyright (c) 1993-2012, The Open Group. All rights reserved.
 *
 * These libraries and programs are free software; you can
 * redistribute them and/or modify them under the terms of the GNU
 * Lesser General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * These libraries and programs are distributed in the hope that
 * they will be useful, but WITHOUT ANY WARRANTY; without even the
 * implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE. See the GNU Lesser General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with these libraries and programs; if not, write
 * to the Free Software Foundation, Inc., 51 Franklin Street, Fifth
 * Floor, Boston, MA 02110-1301 USA
 */
/*
 *+SNOTICE
 *
 *
 *	$TOG: MIMEPartial.C /main/6 1998/04/06 13:27:21 mgreess $
 *
 *	RESTRICTED CONFIDENTIAL INFORMATION:
 *	
 *	The information in this document is subject to special
 *	restrictions in a confidential disclosure agreement bertween
 *	HP, IBM, Sun, USL, SCO and Univel.  Do not distribute this
 *	document outside HP, IBM, Sun, USL, SCO, or Univel wihtout
 *	Sun's specific written approval.  This documment and all copies
 *	and derivative works thereof must be returned or destroyed at
 *	Sun's request.
 *
 *	Copyright 1993 Sun Microsystems, Inc.  All rights reserved.
 *
 *+ENOTICE
 */
#include <EUSCompat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

#include <DtMail/DtMail.hh>
#include "RFCImpl.hh"
#include "str_utils.h"

static const char	* contentType = "content-type";
static const char	* partial = "message/partial";

//
// Limits on putting a message together from its message/partial parts.  The
// numbers in the headers of the parts come from whoever sent them.
//
static const unsigned int	MaxPartialParts = 1000;
static const size_t		MaxPartialMessage = 256UL * 1024 * 1024;

//
// Text of the message under construction.  It can hold any bytes, grows as
// needed up to MaxPartialMessage, and remembers a failure so that a caller can
// append everything and look once.
//
class PartialText
{
  public:
    PartialText() : _data(NULL), _length(0), _capacity(0), _failed(false) { }
    ~PartialText() { free(_data); }

    bool	append(const char * text, size_t length);
    bool	append(const char * text) { return append(text, strlen(text)); }
    bool	failed() const { return _failed; }
    size_t	length() const { return _length; }

    // The text with a terminating NUL byte.  The caller owns the memory.
    char	* release();

  private:
    char	* _data;
    size_t	  _length;
    size_t	  _capacity;
    bool	  _failed;
};

bool
PartialText::append(const char * text, size_t length)
{
  if (_failed)
    return false;
  if (length == 0)
    return true;
  if (length > MaxPartialMessage - _length) {
    _failed = true;
    return false;
  }

  size_t needed = _length + length + 1;
  if (needed > _capacity) {
    size_t capacity = _capacity ? _capacity : 4096;
    while (capacity < needed)
      capacity *= 2;
    char * grown = (char *) realloc(_data, capacity);
    if (grown == NULL) {
      _failed = true;
      return false;
    }
    _data = grown;
    _capacity = capacity;
  }
  memcpy(_data + _length, text, length);
  _length += length;
  _data[_length] = '\0';
  return true;
}

char *
PartialText::release()
{
  if (_data == NULL)
    _data = (char *) calloc(1, 1);
  char * data = _data;
  _data = NULL;
  _length = _capacity = 0;
  return data;
}

//
// Get the named header out of out of the message.
//
static char	*
getNamedHeader(DtMailEnv		& error,
	       const char	* headerName,
	       RFCMessage	* message)
{
  DtMailValueSeq	  value;
  char			* results = NULL;
  RFCEnvelope		* env = (RFCEnvelope *)message->getEnvelope(error);

  if (error.isNotSet()) {
    env->getHeader(error, headerName, DTM_FALSE, value);

    if (error.isNotSet() && value.length() > 0) {
      results = strdup(*(value[0]));
    }
  }

  return(results);
}

//
// Return the value of the named parameter of a header value such as
//   message/partial; id="abc@host"; number=2; total=3
// The result is a copy for the caller to free, or NULL when there is no such
// parameter.  A quoted value loses its quotes, and a backslash in it quotes
// the next character.
//
static char	*
getNamedValueString(const char *string, const char *name)
{
  size_t	  nameLen = strlen(name);
  const char	* cur = strchr(string, ';');	// The parameters follow the type.

  while (cur != NULL && *cur == ';') {
    cur++;
    while (*cur != '\0' && isspace((unsigned char) *cur))
      cur++;

    const char	* nameStart = cur;
    while (*cur != '\0' && *cur != '=' && *cur != ';')
      cur++;
    const char	* nameEnd = cur;
    while (nameEnd > nameStart && isspace((unsigned char) nameEnd[-1]))
      nameEnd--;

    char	* value = NULL;
    if (*cur == '=') {
      cur++;
      while (*cur != '\0' && isspace((unsigned char) *cur))
	cur++;

      value = (char *) malloc(strlen(cur) + 1);
      if (value == NULL)
	return NULL;

      char	* out = value;
      if (*cur == '"') {
	cur++;
	while (*cur != '\0' && *cur != '"') {
	  if (*cur == '\\' && cur[1] != '\0')
	    cur++;
	  *out++ = *cur++;
	}
	if (*cur == '"')
	  cur++;
      }
      else {
	while (*cur != '\0' && *cur != ';')
	  *out++ = *cur++;
	while (out > value && isspace((unsigned char) out[-1]))
	  out--;
      }
      *out = '\0';
    }

    if (value != NULL
	&& (size_t) (nameEnd - nameStart) == nameLen
	&& strncasecmp(nameStart, name, nameLen) == 0) {
      return value;
    }
    free(value);

    while (*cur != '\0' && *cur != ';')
      cur++;
  }
  return NULL;
}

//
// Return the number in the named parameter of a header value.  Zero stands for
// "not there", and for anything that is not a plain number up to INT_MAX.
//
static unsigned int
getNamedValueInt(const char *string, const char *name)
{
  unsigned int	  results = 0;
  char		* text = getNamedValueString(string, name);

  if (text != NULL) {
    char	* end = NULL;

    errno = 0;
    long number = strtol(text, &end, 10);
    while (end != NULL && *end != '\0' && isspace((unsigned char) *end))
      end++;
    if (errno == 0 && end != text && *end == '\0'
	&& number > 0 && number <= INT_MAX)
      results = (unsigned int) number;
    free(text);
  }
  return(results);
}

//
// Where the first blank line of the text is (the end of the header lines
// before it), or NULL.  The text need not be terminated.
//
static const char *
findBlankLine(const char * text, size_t length)
{
  for (size_t at = 0; at + 1 < length; at++) {
    if (text[at] == '\n' && text[at + 1] == '\n')
      return &text[at];
  }
  return NULL;
}

DtMailBoolean
RFCMailBox::_isPartial(DtMailEnv	& error,
		       RFCMessage	* message)
{
  DtMailBoolean		  results = DTM_FALSE;

  char		* type;

  if (message != NULL
      && (type = getNamedHeader(error, contentType, message)) != NULL) {
    
    if (error.isNotSet()) {
      size_t partialLen = strlen(partial);

      if (strncasecmp(type, partial, partialLen) == 0
	  && (type[partialLen] == '\0' || type[partialLen] == ';'
	      || isspace((unsigned char) type[partialLen]))) {

	//
	// Add ourselves to the array.
	//
	_partialData	* newData = new _partialData;

	newData->id = getNamedValueString(type, "id");
	newData->number = getNamedValueInt(type, "number");
	newData->total =  getNamedValueInt(type, "total");
	newData->msg = message;
	//
	// It has to have a number and an ID or it is not a valid
	// message/partial and there would be no way to put it
	// back together. Total is optional except for the last part
	// where it is required.  A part cannot be numbered beyond
	// its own total, and neither can go past what we put together.
	//
	if (newData->number > 0 && newData->number <= MaxPartialParts
	    && newData->total <= MaxPartialParts
	    && (newData->total == 0 || newData->number <= newData->total)
	    && newData->id != NULL) {
	  _partialData ** grown = (_partialData **)realloc(_partialList,
						  sizeof(_partialData *)
						   *(_partialListCount+1));
	  if (grown != NULL) {
	    _partialList = grown;
	    _partialList[_partialListCount] = newData;
	    _partialListCount++;
	    results = DTM_TRUE;
	  }
	  else {
	    free((void *) newData->id);
	    delete newData;
	  }
	} else {
	  free((void *) newData->id);
	  delete newData;	// Not a valid message/partial.
	}
      }
      free(type);
    }
    else {
	free(type);
	error.clear();
    }
  }
  return(results);
}

//
// Write the headers of the first part and the headers of the message it
// carries (see RFC 1521, section 7.3.2 below) and then the data of all of the
// parts into one text.  False when the parts cannot be put together.
//
static bool
buildPartialText(DtMailEnv	& error,
		 RFCMessage	** messages,
		 unsigned int	  totalParts,
		 PartialText	& text)
{
  //
  // From RFC 1521, section 7.3.2:
  //
  // When generating and reassembling the parts of a message/partial
  // message, the headers of the encapsulated message must be merged with
  // the headers of the enclosing entities.  In this process the
  // following rules must be observed:
  //
  // (1) All of the header fields from the initial enclosing entity
  // (part one), except those that start with "Content-" and the
  // specific header fields "Message-ID", "Encrypted", and
  // "MIME-Version", must be copied, in order, to the new message.
  //
  // (2) Only those header fields in the enclosed message which start
  // with "Content-" and "Message-ID", "Encrypted", and "MIME-Version"
  // must be appended, in order, to the header fields of the new
  // message.  Any header fields in the enclosed message which do not
  // start with "Content-" (except for "Message-ID", "Encrypted", and
  // "MIME-Version") will be ignored.
  //
  // (3) All of the header fields from the second and any subsequent
  // messages will be ignored.

  RFCEnvelope		* env
    = (RFCEnvelope *)messages[0]->getEnvelope(error);
  if (error.isSet())
    return false;

  DtMailHeaderHandle	  headerHandle;
  DtMailValueSeq	  headerValue;
  char			* headerName;
  const char		* unixFromLine = NULL;

  unsigned int	  	  duplicateCount;
  int			  fromLen = 0;
  int			  headerNumber = 0;
  unsigned int	  	  dupOffset;

  //
  // Get the first header.
  //
  headerHandle = env->getFirstHeader(error, &headerName, headerValue);
  if (error.isSet() || headerHandle == NULL)
    return false;

  unixFromLine = env->unixFrom(error, fromLen);
  if (unixFromLine != NULL && fromLen > 0) {
    text.append(unixFromLine, (size_t) fromLen);
  }
  if (error.isSet())
    return false;

  //
  // Keep geting more (all) of the headers.
  //
  do {
    if ((strncasecmp(headerName, "Content-", 8) != 0)
	&& (strcasecmp(headerName, "Message-ID") != 0)
	&& (strcasecmp(headerName, "Encrypted") != 0)
	&& (strcasecmp(headerName, "MIME-Version") != 0)
	&& (strcasecmp(headerName, "Status") != 0)) {

      //
      // If there is more than one value for each type,
      // then get all of the values.
      //
      duplicateCount = headerValue.length();
      for (dupOffset=0; dupOffset < duplicateCount; dupOffset++ ) {

	//
	// Skip if the first line is a from.
	//
	if (unixFromLine != NULL && headerNumber == 0) {
	  continue;
	}
	text.append(headerName);
	text.append(": ");
	text.append(*(headerValue[dupOffset]));
	text.append("\n");
      }
    }

    //
    // Clear out the current one, so that getNextHeader
    // does not append.  The name was allocated for us.
    //
    headerValue.clear();
    free(headerName);
    headerName = NULL;

    headerHandle = env->getNextHeader(error,
				      headerHandle,
				      &headerName,
				      headerValue);

    headerNumber++;
    //
    // getNextHeader returns NULL where there are no more.
    //
  } while(error.isNotSet() && headerHandle != NULL);

  headerValue.clear();
  if (error.isSet())
    return false;

  //
  // Now add in the headers from the imbeded message in part 1.
  // To do this we need to convert the message to an envelope
  // and then get the headers from that.
  //
  // We then delete the envelope as it is not longer needed.
  //
  RFCBodyPart		* bp
    = (RFCBodyPart *)messages[0]->getFirstBodyPart(error);
  if (error.isSet() || bp == NULL)
    return false;

  unsigned long		  length = 0;
  const void		* contents = NULL;

  //
  // Get the body part of the first message. It contains
  // the headers that we need to merge with.
  //
  bp->getContents(error, &contents, &length, NULL, NULL, NULL, NULL);
  if (error.isSet() || contents == NULL)
    return false;

  //
  // Turn the 1st messages body into an envelope.
  //
  // Headers end with two new lines.
  //
  const char	* embHeader1St = (const char *)contents;
  const char	* endHeader = findBlankLine(embHeader1St, length);
  if (endHeader == NULL || length > (unsigned long) INT_MAX)
    return false;

  // The first message is the parent: reading a header value needs its session.
  RFCEnvelope	embEnv(error,
		       messages[0],
		       embHeader1St,
		       (int)(endHeader - embHeader1St));
  if (error.isSet())
    return false;

  //
  // Get the first header from the embeded message.
  //
  headerHandle = embEnv.getFirstHeader(error, &headerName, headerValue);
  if (error.isSet())
    return false;

  //
  // Keep geting more (all) of the headers.
  //
  while (headerHandle != NULL) {
    if ((strncasecmp(headerName, "Content-", 8) == 0)
	|| (strcasecmp(headerName, "Message-ID") == 0)
	|| (strcasecmp(headerName, "Encrypted") == 0)
	|| (strcasecmp(headerName, "MIME-Version") == 0)) {

      //
      // If there is more than one value for each type,
      // then get all of the values.
      //
      duplicateCount = headerValue.length();
      for (dupOffset=0; dupOffset < duplicateCount; dupOffset++ ) {
	text.append(headerName);
	text.append(": ");
	text.append(*(headerValue[dupOffset]));
	text.append("\n");
      }
    }

    //
    // Clear out the current one, so that getNextHeader
    // does not append.  The name was allocated for us.
    //
    headerValue.clear();
    free(headerName);
    headerName = NULL;
    headerHandle = embEnv.getNextHeader(error,
					headerHandle,
					&headerName,
					headerValue);
    //
    // getNextHeader returns NULL where there are no more.
    //
    if (error.isSet())
      return false;
  }
  headerValue.clear();
  text.append("\n");	// End of headers.

  //
  // Now add the data parts into the one part: what follows the blank line
  // of the first part, then the 2nd -> Nth body parts.
  //
  const char	* data = endHeader + 2;
  text.append(data, length - (size_t) (data - embHeader1St));

  for (unsigned int offset = 1; offset < totalParts ; offset++) {
    bp = (RFCBodyPart *)messages[offset]->getFirstBodyPart(error);
    if (error.isSet() || bp == NULL)
      return false;

    bp->getContents(error, &contents, &length, NULL, NULL, NULL, NULL);
    if (error.isSet() || (contents == NULL && length > 0))
      return false;

    text.append((const char *)contents, (size_t) length);
  }

  return !text.failed();
}

RFCMessage	*
RFCMailBox::_assemblePartial(DtMailEnv	& error,
			     RFCMessage	* message)
{
  unsigned int	  offset;
  unsigned int	  totalParts = 0;

  RFCMessage		* msg = message;

  _partialData		* data = NULL;

  if (message == NULL)
    return(msg);

  //
  // Find ourselfs in the list.
  //
  for (offset = 0 ; offset < _partialListCount ; offset++) {
    if (_partialList[offset]->msg == message) {
      data = _partialList[offset];
      break;
    }
  }
  if (data == NULL)
    return(msg);

  //
  // Now look for the total for this message-id.
  //
  for (offset = 0 ; offset < _partialListCount ; offset++) {
    if (strcasecmp(_partialList[offset]->id, data->id) == 0
	&& _partialList[offset]->total > 0) {
      totalParts = _partialList[offset]->total;
      break;
    }
  }

  //
  // If we know how many parts we are to process - start looking.
  //
  // If we do not know the total, then we know that not all of the
  // parts have arrived. (the last partial message must have
  // the total in it).
  //
  if (totalParts == 0 || totalParts > MaxPartialParts)
    return(msg);

  RFCMessage	** messages
    = (RFCMessage **)calloc(totalParts, sizeof(RFCMessage *));
  if (messages == NULL)
    return(msg);

  //
  // Now we go thru the list again, this time looking for
  // all of the parts. Place them in the correct order
  // in the list.  A part numbered beyond the total does not belong to
  // the message, and of two parts with one number the first one counts.
  //
  for (offset = 0 ; offset < _partialListCount ; offset++) {
    _partialData	* part = _partialList[offset];

    if (strcasecmp(part->id, data->id) == 0
	&& part->number >= 1 && part->number <= totalParts
	&& messages[part->number - 1] == NULL) {
      messages[part->number - 1] = part->msg;
    }
  }

  //
  // If all of the parts are IN, then assemble them into
  // a new message.
  //
  for (offset = 0 ; offset < totalParts ; offset++) {
    if (messages[offset] == NULL) {
      break;	// Found a missing part, do not assemble.
    }
  }

  if (offset == totalParts) {	// Did we find all of the parts ?
    PartialText	  text;

    if (buildPartialText(error, messages, totalParts, text)) {
      //
      // Make the new Message.  It reads its headers and bodies from the
      // text when they are asked for, so the text stays allocated.
      //
      size_t		  messageSize = text.length();
      char		* newMessage = text.release();
      const char	* start = newMessage;

      msg = new RFCMessage(error, this, &start, newMessage + messageSize);

      if (error.isSet()) {
	delete msg;
	free(newMessage);
	msg = message;
      }
      else {
	for (offset = 0; offset < totalParts ; offset++) {
	  //
	  // Delete the old parts.
	  //
	  messages[offset]->setFlag(error, DtMailMessageDeletePending);

	  //
	  // TODO - CALL EXPUNGE to get rid of the message that
	  // we just marked for delete !!!!!
	  //
	}
      }
    }
    // The parts stay as they are when they cannot be put together.
    error.clear();
  }

  free(messages);
  return(msg);	// This could be the new or the original message.
}
