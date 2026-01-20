Got it. Your current code mixes these concepts:

* `st_size` = **apparent size** (what `ls -l` shows)
* `st_blocks*512` = **actual disk usage** (what `du` approximates)

With hardlinks, **apparent size per name** stays the same, but **actual disk blocks must be counted once per (dev,inode)**.

### What’s “wrong” today

You always add both `st_size` and `st_blocks` for every `S_ISREG`, so hardlinked files inflate *both*.

---

## What to implement

### 1) Track three new stats

Add to `StorageSizeData`:

* `unsigned long hardlink_name_count;`
  number of directory entries (names) that are hardlinks (`st_nlink > 1`)
* `unsigned long hardlink_unique_inode_count;`
  number of **unique** hardlinked inodes encountered
* `unsigned long hardlink_extra_name_count;`
  number of *additional* names beyond the first (duplicates)

And you need a “seen inode” set keyed by `(st_dev, st_ino)`.

---

## How to change the accounting

### A) Always add to **apparent size**

Because you want apparent size as “sum of names”:

* for every regular file name:

  * `apparent_size += st_size`

### B) Add to **actual disk usage** only once per inode

* for regular files:

  * if `(st_dev, st_ino)` not seen yet:

    * `actual_blocks += st_blocks*512`
    * mark seen

### C) Count “files with more than one file name”

This is exactly “unique inodes with `st_nlink > 1` that you encountered”:

* if `st_nlink > 1` and inode not seen yet:

  * `hardlink_unique_inode_count++`

And count how many names are hardlink names:

* if `st_nlink > 1`:

  * `hardlink_name_count++`

And how many are duplicates (extra names):

* if `st_nlink > 1` and inode already seen:

  * `hardlink_extra_name_count++`

---

## Where to patch in your code

Right here:

```c
else if (S_ISREG(st.st_mode))
{
    data->regular_file_count++;
    StorageSizeAddEntrySize(data, &st, False);
}
```

Replace it with logic like this (conceptually):

```c
else if (S_ISREG(st.st_mode))
{
    data->regular_file_count++;

    /* apparent size: count every filename */
    data->apparent_size += (unsigned long long)st.st_size;

    /* hardlink stats */
    if (st.st_nlink > 1)
        data->hardlink_name_count++;

    /* actual disk usage: count once per (dev,inode) */
    if (!SeenInode(data, st.st_dev, st.st_ino))
    {
        MarkSeenInode(data, st.st_dev, st.st_ino);

        data->actual_blocks += (unsigned long long)st.st_blocks * 512ULL;

        if (st.st_nlink > 1)
            data->hardlink_unique_inode_count++;
    }
    else
    {
        if (st.st_nlink > 1)
            data->hardlink_extra_name_count++;
    }
}
```

Then update your UI labels:

* “Total size” → show **apparent size**
* “Storage used” → show **actual disk usage**
* Add a new row like:
  **“Hardlinked files:”** = `hardlink_unique_inode_count`
  (optionally also show “extra names”)

Example text:

* `Hardlinked files: 123 (456 additional names)`

---

## Important detail: keep `(st_dev, st_ino)`

Hardlinks don’t cross filesystems, but your traversal might, and inode numbers can repeat on different devices. So the key must be both.

---

If you want, I can give you a small, self-contained hash-set implementation (open addressing) that fits this file and uses `XtMalloc/XtFree` so it matches the codebase style.
