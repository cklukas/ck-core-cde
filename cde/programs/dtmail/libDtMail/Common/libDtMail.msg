$ +SNOTICE
$ 
$ 	$TOG: libDtMail.msg /main/17 1999/03/26 16:52:56 mgreess $
$ 
$ 	RESTRICTED CONFIDENTIAL INFORMATION:
$ 	
$ 	The information in this document is subject to special
$ 	restrictions in a confidential disclosure agreement bertween
$ 	HP, IBM, Sun, USL, SCO and Univel.  Do not distribute this
$ 	document outside HP, IBM, Sun, USL, SCO, or Univel wihtout
$ 	Sun's specific written approval.  This documment and all copies
$ 	and derivative works thereof must be returned or destroyed at
$ 	Sun's request.
$ 
$ 	Copyright 1993, 1994, 1995 Sun Microsystems, Inc.  All rights reserved.
$ 
$ +ENOTICE
$ @(#)libDtMail.msg	1.9 23 May 1995

$quote "

$set 1 libDtMail API set
$ // DTME_NoError
1 "No error occurred."

$ // DTME_AlreadyLocked
2 "The folder is locked by another session."

$ // DTME_BadArg
3 "A bad argument was passed as a parameter to the operation."

$ // DTME_BadMailAddress
4 "The specified mail address could not be processed."

$ // DTME_FileCreateFailed
5 "The requested file could not be created."

$ // DTME_FolderNotSet
6 "The mailbox to incorporate was not set."

$ // DTME_GetLockRefused
7 "The user refused to take the lock of a locked mailbox."

$ // DTME_ImplFailure
8 "The specified implementation could not perform the requested operation."

$ // DTME_InitializationFailed
9 "The instance could not be initialized."

$ // DTME_InternalFailure
10 "An internal failure occurred while performing the operation."

$ // DTME_InvalidError
11 "The error structure is invalid."

$ // DTME_InvalidOperationSequence
12 "An operation was attempted before the instance was initialized."

$ // DTME_MD5ChecksumFailed
13 "The MD5 signature did not match the message contents."

$ // DTME_MailTransportFailed
14 "Unable to communicate with the mail transport."

$ // DTME_NoDataType
15 "There is no data type that matches the transport type."

$ // DTME_NoImplementations
16 "No implementations were found for the mail library."

$ // DTME_NoMailBox
17 "The mailbox was uninitialized at load time."

$ // DTME_NoMemory
18 "No memory available for operation."

$ // DTME_NoMsgCat
19 "No message catalog exists for DtMail."

$ // DTME_NoNewMail
20 "There is no new mail to incorporate."

$ // DTME_NoObjectValue
21 "No value for the object could be found."

$ // DTME_NoSuchFile
22 "The mailbox does not exist and creation was not requested."

$ // DTME_NoSuchImplementation
23 "The specified implementation does not exist."

$ // DTME_NoSuchType
24 "The data type is not known to the library."

$ // DTME_NoUser
25 "The user for this session could not be identified."

$ // DTME_NotInbox
26 "The file specified is not a mailbox."

$ // DTME_NotLocked
27 "The mailbox is not locked for access."

$ // DTME_NotMailBox
28 "The requested file is not a mailbox in any format recognized by this implementation."

$ // DTME_NotSupported
29 "The operation is not supported by the current implementation."

$ // DTME_ObjectAccessFailed
30 "Unable to access an object required to complete the operation."

$ // DTME_ObjectCreationFailed
31 "Unable to create an object required to complete the operation."

$ // DTME_ObjectInUse
32 "An attempt was made to initialize an object that was already initialized."

$ // DTME_ObjectInvalid
33 "An invalid object was referenced."

$ // DTME_OpenContainerInterrupted
34 "The user interrupted the process of opening a mailbox."

$ // DTME_OperationInvalid
35 "An internal error occurred while performing the operation."

$ // DTME_OtherOwnsWrite
36 "Another mail program owns the write access to the mailbox."

$ // DTME_RequestDenied
37 "The other session denied the request for lock or copy."

$ // DTME_TTFailure
38 "A ToolTalk message could not be processed."

$ // DTME_TransportFailed
39 "The mail delivery transport failed."

$ // DTME_UnknownFormat
40 "The message is not in one of the supported formats."

$ // DTME_UnknownOpenError
41 "An unknown error occurred when opening a mailbox."

$ // DTME_UserAbort
42 "The user aborted the operation."

$ // DTME_UserInterrupted
43 "The user interrupted the operation."

$ // DTME_ObjectReadOnly
44 "The mailbox permissions only allow read access."

$ // DTME_NoPermission
45 "The user does not have access to the mailbox."

$ // DTME_IsDirectory
46 "The specified path is a directory."

$ // DTME_CannotRemoveStaleMailboxLockFile
47 "Cannot lock mailbox (could not remove stale lock file).\nStale lock file: %s\nReason: %s"

$ // DTME_CannotCreateMailboxLockFile
48 "Cannot lock mailbox (could not create lock file).\nMailbox lock file: %s\nReason: %s"

$ // DTME_CannotCreateMailboxLockFile_NoPermission
49 "Cannot create lock file and lock mailbox because the user does not have\naccess to either the mailbox or the directory containing the mailbox."

$ // DTME_CannotCreateMailboxLockFile_IsDirectory
50 "Cannot create lock file and lock mailbox because the name for the\nmailbox lock file already exists and is a directory."

$ // DTME_CannotCreateMailboxLockFile_NoSuchFile
51 "Cannot create lock file and lock mailbox because a component of the\npath name of the lock file is not a directory."

$ // DTME_CannotCreateMailboxLockFile_RemoteAccessLost
52 "Cannot create lock file and lock mailbox because the remote system on\nwhich the lock file was to be created is no longer accessible."

$ // DTME_CannotObtainInformationOnOpenMailboxFile
53 "Cannot obtain information on current mailbox file.\nMailbox file: %s\nReason: %s"

$ // DTME_CannotCreateTemporaryMailboxFile
54 "Cannot create temporary mailbox file.\nTemporary Mailbox file: %s\nReason: %s"

$ // DTME_CannotCreateTemporaryMailboxFile_NoPermission
55 "Cannot create temporary mailbox file because the user does not have\naccess to either the mailbox or the directory containing the mailbox."

$ // DTME_CannotCreateTemporaryMailboxFile_IsDirectory
56 "Cannot create temporary mailbox file because the name for the\ntemporary mailbox already exists and is a directory."

$ // DTME_CannotCreateTemporaryMailboxFile_NoSuchFile
57 "Cannot create temporary mailbox file because a component of the\npath name of the temporary file is not a directory."

$ // DTME_CannotCreateTemporaryMailboxFile_RemoteAccessLost
58 "Cannot create temporary mailbox file because the remote system on\nwhich the file was to be created is no longer accessible."

$ // DTME_CannotSetPermissionsOfTemporaryMailboxFile
59 "Cannot set permissions on temporary mailbox file.\nTemporary Mailbox file: %s\nPermissions requested: %o\nReason: %s"

$ // DTME_CannotSetOwnerOfTemporaryMailboxFile
60 "Cannot set owner of temporary mailbox file.\nTemporary Mailbox file: %s\nOwner uid requested: %d\nReason: %s"

$ // DTME_CannotSetGroupOfTemporaryMailboxFile
61 "Cannot set group of temporary mailbox file.\nTemporary Mailbox file: %s\nGroup gid requested: %d\nReason: %s"

$ // DTME_CannotWriteToTemporaryMailboxFile
62 "Cannot write to temporary mailbox file.\nTemporary Mailbox file: %s\nReason: %s"

$ // DTME_CannotWriteToTemporaryMailboxFile_ProcessLimitsExceeded
63 "Cannot write to temporary mailbox file because the process's file\nsize limit or the maximum file size has been reached."

$ // DTME_CannotWriteToTemporaryMailboxFile_RemoteAccessLost
64 "Cannot write to temporary mailbox file because the remote system on\nwhich the file was created is no longer accessible."

$ // DTME_CannotWriteToTemporaryMailboxFile_NoFreeSpaceLeft
65 "Cannot write to temporary mailbox file because there is no free\nspace left on the device on which the file was created."

$ // DTME_CannotReadNewMailboxFile
66 "Cannot read new mailbox file\nReason: %s"

$ // DTME_CannotReadNewMailboxFile_OutOfMemory
67 "Cannot read new mailbox file because no memory is available for the operation."

$ // DTME_CannotRemoveMailboxLockFile
68 "Cannot unlock mailbox (could not remove lock file).\nMailbox lock file: %s\nReason: %s"

$ // DTME_CannotRenameNewMailboxFileOverOld
69 "Cannot rename new mailbox file over old mailbox file.\nOld mailbox file still exists but complete and correct contents\nof mailbox contents have been saved in the new mailbox file.\nThis problem must be corrected manually as soon as possible.\nOld Mailbox file: %s\nNew Mailbox file: %s\nReason: %s"

$ // DTME_InternalAssertionFailure
70 "An internal error has occurred within this application.\nThere is no way to recover and continue from this error.\nError condition: %s\n"

$ "-NEW"
$ // - DTME_ResourceParsingNoEndif
$ 
71 "An error occurred while parsing the .mailrc resource file.\nThere is a conditional if statement that does not have a corresponding endif statement.\n"

$ "-NEW"
$ // - DTME_AlreadyOpened
$ 
72 "This mail folder is already opened."

$ "-NEW"
$ // - DTME_OutOfSpace
$ 
73 "No Space on Temporary Filesystem."

$ "-NEW"
$ // - DTME_CannotCreateMailboxDotDtmailLockFile
74 "Mailer has detected a mailbox lockfile:\n%s"

$ "-NEW"
$ // - DTME_MailboxInodeChanged
75 "Mailer can no longer access this mailbox.\nIt would be best to close and reopen it."

$ "-NEW"
$ // - DTME_MailServerAccess_AuthorizationFailed
76 "Cannot retrieve mail for '%s@%s' using '%s'.\nAuthorization failed.  The server returned:\n     %s\n\nPlease enter a new password"

$ "-NEW"
$ // - DTME_MailServerAccess_Error
77 "Cannot retrieve mail for '%s@%s' using '%s'.\nThe server returned:\n     %s"

$ "-NEW"
$ // - DTME_MailServerAccess_MissingPassword
78 "Please enter a password for '%s@%s' using '%s'"

$ "-NEW"
$ // - DTME_MailServerAccess_ProtocolViolation
79 "Cannot retrieve mail for '%s@%s' using '%s'.\nClient/server protocol error."

$ "-NEW"
$ // - DTME_MailServerAccess_ServerTimeoutError
80 "Cannot retrieve mail for '%s@%s' using '%s'.\nTimed out waiting for server."

$ "-NEW"
$ // - DTME_MailServerAccess_SocketIOError
81 "Cannot retrieve mail for '%s@%s' using '%s'.\n%s."

$ "-NEW"
$ // - DTME_AppendMailboxFile_Error
82 "Cannot append to mailbox"

$ "-NEW"
$ // - DTME_AppendMailboxFile_FileTooBig
83 "Cannot append to mailbox:  %s\nSYSERROR(%d):  %s."

$ "-NEW"
$ // - DTME_AppendMailboxFile_LinkLost
84 "Cannot append to mailbox:  %s\nSYSERROR(%d):  %s."

$ "-NEW"
$ // - DTME_AppendMailboxFile_NoSpaceLeft
85 "Cannot append to mailbox:  %s\nSYSERROR(%d):  %s."

$ "-NEW"
$ // - DTME_AppendMailboxFile_SystemError
86 "Cannot append to mailbox:  %s\nSYSERROR(%d):  %s."

$ "-NEW"
$ // - DTME_GetmailCommandRetrieval_SystemError
87 "Getmail command failed:  %s\nSYSERROR(%d):  %s."

$ "-NEW"
$ // - DTME_GetmailCommandRetrieval_AbnormalExit
88 "Getmail command exited abnormally:  %s."

$ "-NEW"
$ // - DTME_PathElementPermissions
89 "Search permission denied on a component of the path prefix,\n'%s'."

$ "-NEW"
$ // - DTME_PathElementNotDirectory
90 "A component of the path prefix is not a directory,\n'%s'."

$ "-NEW"
$ // - DTME_PathElementDoesNotExist
91 "A component of the path prefix does not exist,\n'%s'."

$ "-NEW"
$ // - DTME_MailServerAccessInfo_SocketOpen
92 "Opening connection for '%s@%s'."

$ "-NEW"
$ // - DTME_MailServerAccessInfo_NoMessages
93 "No messages for '%s@%s'."

$ "-NEW"
$ // - DTME_MailServerAccessInfo_RetrievingMessage
94 "Retrieving message %d of %d for '%s@%s'."

$ "-NEW"
$ // - DTME_MailServerAccessInfo_MessageTooLarge
95 "Skipping oversized message (%d bytes)."


$ 
$ Strings from libDtMail/Common/DtMailValuesBuiltin.C

$set 2
$ /* NL_COMMENT
$  * The %C is the time and date format, please refer to strftime man page for 
$  * explanation of each format.
$  */
1 "%C" 


$ 
$ SockOpen error strings from libDtMail/Common/IO.C

$ "-NEW"
2 "Unknown host:  %s"

$ "-NEW"
3 "Error creating socket: %s"

$ "-NEW"
4 "Error connecting to socket:  %s"


$ 
$ FileShare error strings from libDtMail/Common/FileShare.C

$ "-NEW"
5 "Another user would like your lock."

$ "-NEW"
6 "Another session has this mailbox locked. Request access?"

$ "-NEW"
7 "Unable to obtain lock, open this mailbox as read only?"

$ "-NEW"
8 "Unable to obtain lock because system not responding, open this mailbox as read only, read write, or cancel?"

$ "-NEW"
9 "Another user has taken your lock."
