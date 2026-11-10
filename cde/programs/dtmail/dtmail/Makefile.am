MAINTAINERCLEANFILES = Makefile.in

bin_PROGRAMS = dtmail

dtmail_CXXFLAGS = -I../include -I../include/MotifApp -I../include/utils \
		  -I../include/DtMail -I../libDtMail/RFC $(X_CFLAGS) \
		  -I$(top_srcdir)/programs/dtmail/include \
		  -I$(top_srcdir)/programs/dtmail/include/MotifApp \
		  -I$(top_srcdir)/programs/dtmail/include/utils \
		  -I$(top_srcdir)/programs/dtmail/include/DtMail \
		  -I$(top_srcdir)/programs/dtmail/libDtMail/RFC \
		  -I$(srcdir)/lib -I../../dthelp -DRELEASE_NOTES \
		  -I$(top_srcdir)/programs/dthelp \
		  -DDTMAIL_TOOLTALK -DDTEDITOR

dtmail_LDADD = ../libDtMail/libDtMail.a ../MotifApp/libMotifApp.a \
		$(DTCLIENTLIBS) $(XTOOLLIB) $(TIRPCLIB)

dtmail_SOURCES = AliasListUiItem.C               AlternatesListUiItem.C \
		 AntiCheckBoxUiItem.C            AttachArea.C \
		 Attachment.C			 CheckBoxUiItem.C \
		 CheckForMailUiItem.C		 ComposeCmds.C \
		 CustomListUiItem.C		 Dialog.C \
		 DialogShell.C			 DmxMailbox.C \
		 DmxMessage.C			 DmxPrintJob.C \
		 DmxPrintOptions.C		 DmxPrintOutput.C \
                 DmxPrintSetup.C		 DmxUtils.C \
		 DtEditor.C			 DtMailEditor.C \
		 DtMailGenDialog.C		 DtMailWDM.C \
		 Editor.C			 EncryptedTextFieldUiItem.C \
		 FindDialog.C			 Fonts.C \
		 Icon.C				 IgnoreListUiItem.C \
		 InboxTextFieldUiItem.C		 \
		 IndexedOptionMenu.C		 IndexedOptionMenuUiItem.C \
		 ListUiItem.C			 MailRcSource.C \
		 MailRetrievalOptions.C		 MailSession.C \
		 MoveMenuListUiItem.C            MsgHndArray.C \
		 MsgScrollingList.C              NoOpCmd.C \
		 OptCmd.C                        PasswordDialogManager.C \
		 PropUi.C                        \
		 RoamApp.C                       RoamCmds.C \
		 RoamInterruptibleCmd.C          RoamMenuWindow.C \
		 ScaleUiItem.C                   SendMsgDialog.C \
		 Sort.C                          SortCmd.C \
		 SpinBoxUiItem.C                 StringTab.c \
		 TemplateListUiItem.C            TextFieldUiItem.C \
		 Undelete.C                      ViewMsgDialog.C \
		 WMSaveSession.C                 XmStrCollector.C \
		 XmTextEditor.C                  XtArgCollector.C \
		 dtb_utils.C                     options_stubs.C \
		 options_ui.C                    options_util.C
