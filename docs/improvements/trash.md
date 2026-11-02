---
title: Trash improvements
description: Better Trash metadata, desktop state and freedesktop.org interoperability.
---

# Trash improvements

CK Core CDE records original paths, deletion dates and sizes for trashed
objects, and can import entries from the freedesktop.org Trash layout. File
operations use secure temporary files and handle longer paths more reliably.

The desktop icon reflects whether Trash contains items. Trash actions can also
raise and focus the existing Trash window instead of opening a competing view.

![CDE Trash Can window and its corresponding open-window desktop icon](../images/features/trash.png)

*The managed Trash view and its workspace icon in the uConsole session.*

Together these changes make the desktop indicator, window and stored metadata
describe the same Trash state.
