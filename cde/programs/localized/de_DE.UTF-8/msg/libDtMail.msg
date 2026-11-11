
$ +SNOTICE
$ 
$ 	$TOG: libDtMail.msg /main/18 1999/03/26 16:45:33 mgreess $
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
1 "Kein Fehler."

$ // DTME_AlreadyLocked
2 "Der Ordner wurde von einer anderen Session gesperrt."

$ // DTME_BadArg
3 "Ein fehlerhaftes Argument wurde als Parameter an die Funktion übergeben."

$ // DTME_BadMailAddress
4 "Die angegebene Postadresse konnte nicht verarbeitet werden."

$ // DTME_FileCreateFailed
5 "Die angeforderte Datei konnte nicht erstellt werden."

$ // DTME_FolderNotSet
6 "Der aufzunehmende Briefkasten wurde nicht festgelegt."

$ // DTME_GetLockRefused
7 "Der Benutzer weigerte sich, die Sperrung eines Briefkastens zu entfernen."

$ // DTME_ImplFailure
8 "Die angegebene Implementierung konnte die angeforderte Operation nicht durchführen."

$ // DTME_InitializationFailed
9 "Die instance konnte nicht initialisiert werden."

$ // DTME_InternalFailure
10 "Interner Fehler während der Durchführung der Operation."

$ // DTME_InvalidError
11 "Die Fehlerstruktur ist ungültig."

$ // DTME_InvalidOperationSequence
12 "Eine Operation wurde versucht, bevor die instance initialisiert war."

$ // DTME_MD5ChecksumFailed
13 "Die MD5 signature stimmte nicht mit dem Nachrichtinhalt überein."

$ // DTME_MailTransportFailed
14 "Kann nicht mit dem Posttransport kommunizieren."

$ // DTME_NoDataType
15 "Kein Datentyp stimmt mit dem Transporttyp überein."

$ // DTME_NoImplementations
16 "Keine Implementierungen für die Postbibliothek gefunden."

$ // DTME_NoMailBox
17 "Der Briefkasten war zur Ladezeit nicht initialisiert."

$ // DTME_NoMemory
18 "Kein Speicher für die Operation verfügbar."

$ // DTME_NoMsgCat
19 "Für DtMail existiert kein Meldungskatalog."

$ // DTME_NoNewMail
20 "Keine neue Post aufzunehmen."

$ // DTME_NoObjectValue
21 "Für das Objekt konnte kein Wert gefunden werden."

$ // DTME_NoSuchFile
22 "Der Briefkasten existiert nicht, und die Erstellung wurde nicht angefordert."

$ // DTME_NoSuchImplementation
23 "Die angegebene Implementierung existiert nicht."

$ // DTME_NoSuchType
24 "Der Datentyp ist der Bibliothek nicht bekannt."

$ // DTME_NoUser
25 "Der Benutzer für diese Session konnte nicht identifiziert werden."

$ // DTME_NotInbox
26 "Die angegebene Datei ist kein Briefkasten."

$ // DTME_NotLocked
27 "Der Briefkasten ist nicht für den Zugriff gesperrt."

$ // DTME_NotMailBox
28 "Die angeforderte Datei ist in keinem Format, das von dieser Implementierung erkannt wird, ein Briefkasten."

$ // DTME_NotSupported
29 "Diese Operation wird von der aktuellen Implementierung nicht unterstützt."

$ // DTME_ObjectAccessFailed
30 "Kann nicht auf ein Objekt zugreifen, das zur Beendung der Operation erforderlich ist."

$ // DTME_ObjectCreationFailed
31 "Kann ein Objekt nicht erstellen, das zur Beendung der Operation erforderlich ist."

$ // DTME_ObjectInUse
32 "Es wurde der Versuch unternommen, ein Objekt zu initialisieren, das bereits initialisiert war."

$ // DTME_ObjectInvalid
33 "Ein ungültiges Objekt wurde referenziert."

$ // DTME_OpenContainerInterrupted
34 "Der Benutzer unterbrach das Öffnen eines Briefkastens."

$ // DTME_OperationInvalid
35 "Interner Fehler während der Durchführung der Operation."

$ // DTME_OtherOwnsWrite
36 "Ein anderes Postprogramm hat Schreibzugriff auf den Briefkasten."

$ // DTME_RequestDenied
37 "Die andere Session verweigerte die Anforderung für Sperren oder Kopieren."

$ // DTME_TTFailure
38 "Eine ToolTalk-Nachricht konnte nicht verarbeitet werden."

$ // DTME_TransportFailed
39 "Postzustellung fehlgeschlagen."

$ // DTME_UnknownFormat
40 "Die Nachricht ist nicht in einem der unterstützten Formate abgefaßt."

$ // DTME_UnknownOpenError
41 "Unbekannter Fehler beim Öffnen eines Briefkastens."

$ // DTME_UserAbort
42 "Der Benutzer brach die Operation ab."

$ // DTME_UserInterrupted
43 "Der Benutzer unterbrach die Operation."

$ // DTME_ObjectReadOnly
44 "Die Briefkasten-Zugriffsrechte erlauben nur Lesezugriff."

$ // DTME_NoPermission
45 "Der Benutzer hat keinen Zugriff auf den Briefkasten."

$ // DTME_IsDirectory
46 "Der angegebene Pfad ist ein Verzeichnis."

$ // DTME_CannotRemoveStaleMailboxLockFile
47 "Kann Briefkasten nicht sperren (konnte alte Sperrdatei nicht entfernen).\nAlte Sperrdatei: %s\nGrund: %s"

$ // DTME_CannotCreateMailboxLockFile
48 "Kann Briefkasten nicht sperren (konnte Sperrdatei nicht erstellen).\nBriefkasten-Sperrdatei: %s\nGrund: %s"

$ // DTME_CannotCreateMailboxLockFile_NoPermission
49 "Kann weder Sperrdatei noch Sperrbriefkasten erstellen, weil der Benutzer keinen Zugriff\nauf den Briefkasten oder das Verzeichnis hat, das den Briefkasten enthält."

$ // DTME_CannotCreateMailboxLockFile_IsDirectory
50 " Kann weder Sperrdatei noch Sperrbriefkasten erstellen, weil der Name für die\nBriefkastensperrdatei bereits existiert und ein Verzeichnis ist."

$ // DTME_CannotCreateMailboxLockFile_NoSuchFile
51 " Kann weder Sperrdatei noch Sperrbriefkasten erstellen, weil eine Komponente des\nPfadnamens der Sperrdatei kein Verzeichnis ist."

$ // DTME_CannotCreateMailboxLockFile_RemoteAccessLost
52 " Kann weder Sperrdatei noch Sperrbriefkasten erstellen, weil das entfernte System, auf\ndem die Sperrdatei erstellt werden sollte, nicht mehr zugänglich ist."

$ // DTME_CannotObtainInformationOnOpenMailboxFile
53 "Kann keine Informationen über die aktuelle Briefkastendatei erhalten.\nBriefkastendatei: %s\nGrund: %s"

$ // DTME_CannotCreateTemporaryMailboxFile
54 "Kann temporäre Briefkastendatei nicht erstellen.\nTemporäre Briefkastendatei: %s\nGrund: %s"

$ // DTME_CannotCreateTemporaryMailboxFile_NoPermission
55 "Kann temporäre Briefkastendatei nicht erstellen, weil der Benutzer keinen Zugriff\nauf den Briefkasten oder das Verzeichnis hat, das den Briefkasten enthält."

$ // DTME_CannotCreateTemporaryMailboxFile_IsDirectory
56 "Kann temporäre Briefkastendatei nicht erstellen, weil der Name für den\ntemporären Briefkasten bereits existiert und ein Verzeichnis ist."

$ // DTME_CannotCreateTemporaryMailboxFile_NoSuchFile
57 "Kann temporäre Briefkastendatei nicht erstellen, weil eine Komponente des\nPfadnamens der temporären Datei kein Verzeichnis ist."

$ // DTME_CannotCreateTemporaryMailboxFile_RemoteAccessLost
58 "Kann temporäre Briefkastendatei nicht erstellen, weil das entfernte System, auf\ndem die Datei erstellt werden sollte, nicht mehr zugänglich ist."

$ // DTME_CannotSetPermissionsOfTemporaryMailboxFile
59 "Kann keine Zugriffsrechte auf die temporäre Briefkastendatei festlegen.\nTemporäre Briefkastendatei: %s\nZugriffsrechte erforderlich: %o\nGrund: %s"

$ // DTME_CannotSetOwnerOfTemporaryMailboxFile
60 "Kann Eigentümer der temporären Briefkastendatei nicht festlegen.\nTemporäre Briefkastendatei: %s\nEigentümer-UID angefordert: %d\nGrund: %s"

$ // DTME_CannotSetGroupOfTemporaryMailboxFile
61 "Kann keine Gruppe der temporären Briefkastendatei festlegen.\nTemporäre Briefkastendatei: %s\nGruppen-GID angefordert: %d\nGrund: %s"

$ // DTME_CannotWriteToTemporaryMailboxFile
62 "Kann nicht in temporäre Briefkastendatei schreiben.\nTemporäre Briefkastendatei: %s\nGrund: %s"

$ // DTME_CannotWriteToTemporaryMailboxFile_ProcessLimitsExceeded
63 "Kann nicht in temporäre Briefkastendatei schreiben, weil die Dateigrößengrenze\nfür den Prozeß oder die maximale Dateigröße erreicht wurde."

$ // DTME_CannotWriteToTemporaryMailboxFile_RemoteAccessLost
64 "Kann nicht in temporäre Briefkastendatei schreiben, weil das entfernte System, auf\ndem die Datei erstellt wurde, nicht mehr zugänglich ist."

$ // DTME_CannotWriteToTemporaryMailboxFile_NoFreeSpaceLeft
65 "Kann nicht in temporäre Briefkastendatei schreiben, weil auf dem Gerät,\nauf dem die Datei erstellt wurde, kein freier Speicherplatz mehr existiert."

$ // DTME_CannotReadNewMailboxFile
66 "Kann neue Briefkastendatei nicht lesen.\nGrund: %s"

$ // DTME_CannotReadNewMailboxFile_OutOfMemory
67 "Kann neue Briefkastendatei nicht lesen, weil für die Operation kein Speicher verfügbar ist."

$ // DTME_CannotRemoveMailboxLockFile
68 "Kann Briefkasten nicht entsperren (konnte Sperrdatei nicht entfernen).\nBriefkasten-Sperrdatei: %s\nGrund: %s"

$ // DTME_CannotRenameNewMailboxFileOverOld
69 "Kann neue Briefkastendatei nicht über alter Briefkastendatei neu benennen.\nAlte Briefkastendatei existiert noch, aber der vollständige und korrekte Briefkasteninhalt\nwurde in der neuen Briefkastendatei gespeichert.\nDieses Problem muß so schnell wie möglich manuell korrigiert werden.\nAlte Briefkastendatei: %s\nNeue Briefkastendatei: %s\nGrund: %s"

$ // DTME_InternalAssertionFailure
70 "Ein interner Fehler ist innerhalb dieser Anwendung aufgetreten.\nEs gibt keine Möglichkeit, diesen Fehler zu beheben und fortzufahren.\nFehlerbedingung: %s\n"

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
