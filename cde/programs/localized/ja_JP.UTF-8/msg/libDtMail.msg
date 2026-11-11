$ +SNOTICE
$ 
$ 	$TOG: libDtMail.msg /main/19 1999/03/26 16:48:27 mgreess $
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
1 "エラーは発生しませんでした。"

$ // DTME_AlreadyLocked
2 "フォルダは別のセッションによりロックされています。"

$ // DTME_BadArg
3 "操作にパラメータとして不正な引き数が渡されました。"

$ // DTME_BadMailAddress
4 "指定のメールアドレスを処理できません。"

$ // DTME_FileCreateFailed
5 "要求されたファイルを作成できません。"

$ // DTME_FolderNotSet
6 "取り入れようとしたメールボックスは設定されていません。"

$ // DTME_GetLockRefused
7 "ユーザはロックされているメールボックスのロック解除を拒否しました。"

$ // DTME_ImplFailure
8 "指定の実装は要求した操作を実行できません。"

$ // DTME_InitializationFailed
9 "インスタンスを初期化できません。"

$ // DTME_InternalFailure
10 "操作を実行中に内部障害が発生しました。"

$ // DTME_InvalidError
11 "エラー構造が無効です。"

$ // DTME_InvalidOperationSequence
12 "インスタンスを初期化する前に操作を実行しようとしました。"

$ // DTME_MD5ChecksumFailed
13 "MD5 シグニチャーがメッセージ内容と一致しません。"

$ // DTME_MailTransportFailed
14 "メールトランスポートと通信できません。"

$ // DTME_NoDataType
15 "トランスポート型と一致するデータ型がありません。"

$ // DTME_NoImplementations
16 "メールライブラリのための実装が見つかりません。"

$ // DTME_NoMailBox
17 "メールボックスが読み込み時に初期化されません。"

$ // DTME_NoMemory
18 "操作に使用できるメモリがありません。"

$ // DTME_NoMsgCat
19 "メールプログラムのメッセージカタログが存在しません。"

$ // DTME_NoNewMail
20 "新規メールで取り入れていないものはありません。"

$ // DTME_NoObjectValue
21 "オブジェクトの値が見つかりません。"

$ // DTME_NoSuchFile
22 "メールボックスは存在せず、生成を要求していません。"

$ // DTME_NoSuchImplementation
23 "指定の実装は存在しません。"

$ // DTME_NoSuchType
24 "データ型はライブラリにありません。"

$ // DTME_NoUser
25 "このセッションのユーザを特定できません。"

$ // DTME_NotInbox
26 "指定のファイルはメールボックスではありません。"

$ // DTME_NotLocked
27 "メールボックスはアクセスをロックしていません。"

$ // DTME_NotMailBox
28 "要求ファイルは、この実装が認識する形式のメールボックスではありません。"

$ // DTME_NotSupported
29 "現在の実装では操作をサポートしていません。"

$ // DTME_ObjectAccessFailed
30 "操作を完了するのに必要なオブジェクトにアクセスできません。"

$ // DTME_ObjectCreationFailed
31 "操作を完了するのに必要なオブジェクトを作成できません。"

$ // DTME_ObjectInUse
32 "初期化済みのオブジェクトを初期化しようとしました。"

$ // DTME_ObjectInvalid
33 "無効なオブジェクトを参照しました。"

$ // DTME_OpenContainerInterrupted
34 "メールボックスを開くプロセスにユーザが割り込みました。"

$ // DTME_OperationInvalid
35 "操作を実行中に内部エラーが発生しました。"

$ // DTME_OtherOwnsWrite
36 "別のメールプログラムがメールボックスへの書き込み権を持っています。"

$ // DTME_RequestDenied
37 "他のセッションは、ロックまたはコピー要求を拒否しました。"

$ // DTME_TTFailure
38 "ToolTalk メッセージを処理できません。"

$ // DTME_TransportFailed
39 "メールデリバリのトランスポートに失敗しました。"

$ // DTME_UnknownFormat
40 "メッセージがサポートされている形式ではありません。"

$ // DTME_UnknownOpenError
41 "メールボックスを開いている時に未知のエラーが発生しました。"

$ // DTME_UserAbort
42 "ユーザが操作を中止しました。"

$ // DTME_UserInterrupted
43 "ユーザが操作に割り込みました。"

$ // DTME_ObjectReadOnly
44 "メールボックスへのアクセス権は、読み取りのみ許可されています。"

$ // DTME_NoPermission
45 "ユーザはメールボックスへのアクセス権を持っていません。"

$ // DTME_IsDirectory
46 "指定のパスはディレクトリです。"

$ // DTME_CannotRemoveStaleMailboxLockFile
47 "メールボックスをロックできません (旧ロックファイルを削除できません)。\n旧ロックファイル: %s\n理由: %s"

$ // DTME_CannotCreateMailboxLockFile
48 "メールボックスをロックできません (ロックファイルを作成できません)。\メールボックスロックファイル: %s\n理由: %s"

$ // DTME_CannotCreateMailboxLockFile_NoPermission
49 "ユーザがメールボックスまたはメールボックスを含むディレクトリにアクセス権を\n持っていないので、ロックファイルおよびロックメールボックスを作成できません。"

$ // DTME_CannotCreateMailboxLockFile_IsDirectory
50 "メールボックスのロックファイル名が既に存在し、ディレクトリであるため、\nロックファイルおよびロックメールボックスを作成できません。"

$ // DTME_CannotCreateMailboxLockFile_NoSuchFile
51 "ロックファイルのパス名のコンポーネントがディレクトリではないので、\nロックファイルおよびロックメールボックスを作成できません。"

$ // DTME_CannotCreateMailboxLockFile_RemoteAccessLost
52 "ロックファイルを作成しようとしていたリモートシステムにアクセスできなくなった\nため、ロックファイルおよびロックメールボックスを作成できません。"

$ // DTME_CannotObtainInformationOnOpenMailboxFile
53 "現在のメールボックスファイルにある情報を取得できません。\nメールボックスファイル: %s\n理由: %s"

$ // DTME_CannotCreateTemporaryMailboxFile
54 "一時メールボックスファイルを作成できません。\nメールボックスファイル: %s\n理由: %s"

$ // DTME_CannotCreateTemporaryMailboxFile_NoPermission
55 "ユーザがメールボックスまたはメールボックスを含むディレクトリにアクセス権を\n持~っていないので、一時メールボックスファイルを作成できません。"

$ // DTME_CannotCreateTemporaryMailboxFile_IsDirectory
56 "一時メールボックス名が既に存在し、ディレクトリであるため、\n一時メールボックスファイルを作成できません。"

$ // DTME_CannotCreateTemporaryMailboxFile_NoSuchFile
57 "一時ファイルのパス名のコンポーネントがディレクトリでないため、\n一時メールボックス~ファイルを作成できません。"

$ // DTME_CannotCreateTemporaryMailboxFile_RemoteAccessLost
58 "ファイルを作成しようとしていたリモートシステムにアクセスできなくなったため、\n一時メールボックスファイルを作成できません。"

$ // DTME_CannotSetPermissionsOfTemporaryMailboxFile
59 "一時メールボックスファイルにアクセス権を設定できません。\n一時メールボックスファイル: %s\n要求したアクセス権: %o\n理由: %s"

$ // DTME_CannotSetOwnerOfTemporaryMailboxFile
60 "一時メールボックスファイルの所有者を設定できません。\n一時メールボックスファイル: %s\n要求した所有者のユーザ ID: %d\n理由: %s"

$ // DTME_CannotSetGroupOfTemporaryMailboxFile
61 "一時メールボックスファイルのグループを設定できません。\n一時メールボックスファイル: %s\n要求したグループのグループ ID: %d\n理由: %s"

$ // DTME_CannotWriteToTemporaryMailboxFile
62 "一時メールボックスファイルに書き込めません。\n一時メールボックスファイル: %s\n理由: %s"

$ // DTME_CannotWriteToTemporaryMailboxFile_ProcessLimitsExceeded
63 "プロセスのファイルサイズの制限またはファイルの最大サイズに達したため、\n一時メールボックスファイルに書き込めません。"

$ // DTME_CannotWriteToTemporaryMailboxFile_RemoteAccessLost
64 "ファイルを作成しようとしていたリモートシステムにアクセスできなくなったため、\n~一時メールボックスファイルに書き込めません。"

$ // DTME_CannotWriteToTemporaryMailboxFile_NoFreeSpaceLeft
65 "ファイルを作成しているデバイス上に空き領域がないため、\n一時メールボックスファイルに書き込めません。"

$ // DTME_CannotReadNewMailboxFile
66 "新規メールボックスファイルを読めません。\n理由: %s"

$ // DTME_CannotReadNewMailboxFile_OutOfMemory
67 "操作に使用できるメモリがないので、新規メールボックスファイルを読めません。"

$ // DTME_CannotRemoveMailboxLockFile
68 "メールボックスをロック解除できません (ロックファイルを削除できません)。\nメールボックスロックファイル: %s\n理由: %s"

$ // DTME_CannotRenameNewMailboxFileOverOld
69 "旧メールボックスファイルを新規メールボックスファイルに置き換え、\n名前を変更することはできません。\n旧メールボックスファイルは存在しますが、メールボックスの内容はすべて\n新規メールボックスファイルに正しく保存されています。\n早急にこの問題を手動で修正する必要があります。\n旧メールボックスファイル: %s\n新規メールボックスファイル: %s\n理由: %s"

$ // DTME_InternalAssertionFailure
70 "このアプリケーション内で内部エラーが発生しました。\nこのエラーを回復し、ここから継続する方法はありません。\nエラー条件: %s\n"

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
