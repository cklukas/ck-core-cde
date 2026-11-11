$ +SNOTICE
$ 
$ 	$TOG: libDtMail.msg /main/4 1997/10/31 15:40:04 rafi $
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
$       Copyright 1993, 1994, 1995 Sun Microsystems, Inc.  All rights reserved.
$ 
$ +ENOTICE
$ @(#)libDtMail.msg     1.9 23 May 1995
$set 1 libDtMail API set
$ // DTME_NoError
1 没有出现错误。

$ // DTME_AlreadyLocked
2 "The folder is locked by another session."

$ // DTME_BadArg
3 将一个错误的实参作为该操作的参数传递。

$ // DTME_BadMailAddress
4 不能处理指定的邮件地址。

$ // DTME_FileCreateFailed
5 不能创建请求的文件。

$ // DTME_FolderNotSet
6 "The mailbox to incorporate was not set."

$ // DTME_GetLockRefused
7 "The user refused to take the lock of a locked mailbox."

$ // DTME_ImplFailure
8 指定的实现不能执行请求的操作。

$ // DTME_InitializationFailed
9 "The instance could not be initialized."

$ // DTME_InternalFailure
10 "An internal failure occurred while performing the operation."

$ // DTME_InvalidError
11 该错误结构是无效的。

$ // DTME_InvalidOperationSequence
12 "An operation was attempted before the instance was initialized."

$ // DTME_MD5ChecksumFailed
13 此 MD5 签名与该消息内容不匹配。

$ // DTME_MailTransportFailed
14 "Unable to communicate with the mail transport."

$ // DTME_NoDataType
15 没有与此传输类型匹配的数据类型。

$ // DTME_NoImplementations
16 没有找到对该邮件库的实现。

$ // DTME_NoMailBox
17 "The mailbox was uninitialized at load time."

$ // DTME_NoMemory
18 无足够内存执行此操作。

$ // DTME_NoMsgCat
19 不存在用于 DtMail 的消息目录。

$ // DTME_NoNewMail
20 "There is no new mail to incorporate."

$ // DTME_NoObjectValue
21 无法找到该对象的值。

$ // DTME_NoSuchFile
22 "The mailbox does not exist and creation was not requested."

$ // DTME_NoSuchImplementation
23 指定的实现不存在。

$ // DTME_NoSuchType
24 本软件库不认识此数据类型。

$ // DTME_NoUser
25 "The user for this session could not be identified."

$ // DTME_NotInbox
26 "The file specified is not a mailbox."

$ // DTME_NotLocked
27 "The mailbox is not locked for access."

$ // DTME_NotMailBox
28 请求的文件不是一个具有可以由本程序识别格式的信箱。

$ // DTME_NotSupported
29 当前实现不支持此操作。

$ // DTME_ObjectAccessFailed
30 "Unable to access an object required to complete the operation."

$ // DTME_ObjectCreationFailed
31 不能创建一个完成此操作的对象。

$ // DTME_ObjectInUse
32 试图对一个已经初始化的对象进行初始化。

$ // DTME_ObjectInvalid
33 访问了一个无效的对象。

$ // DTME_OpenContainerInterrupted
34 "The user interrupted the process of opening a mailbox."

$ // DTME_OperationInvalid
35 在执行该操作时出现内部错误。

$ // DTME_OtherOwnsWrite
36 "Another mail program owns the write access to the mailbox."

$ // DTME_RequestDenied
37 "The other session denied the request for lock or copy."

$ // DTME_TTFailure
38 有一个 ToolTalk 消息不能处理。

$ // DTME_TransportFailed
39 邮件传输失败。

$ // DTME_UnknownFormat
40 "The message is not in one of the supported formats."

$ // DTME_UnknownOpenError
41 "An unknown error occurred when opening a mailbox."

$ // DTME_UserAbort
42 用户终止了此操作。

$ // DTME_UserInterrupted
43 用户中断了此操作。

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
 
$ 
$ Strings from libDtMail/Common/DtMailValuesBuiltin.C

$set 2
$ /* NL_COMMENT
$  * The %C is the time and date format, please refer to strftime man page for 
$  * explanation of each format.
$  */
1 "%C" 

