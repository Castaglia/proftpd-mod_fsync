proftpd-mod_fsync
=================

Status
------
[![GitHub Actions CI Status](https://github.com/Castaglia/proftpd-mod_fsync/actions/workflows/ci.yml/badge.svg?branch=master)](https://github.com/Castaglia/proftpd-mod_fsync/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-GPL-brightgreen.svg)](https://img.shields.io/badge/license-GPL-brightgreen.svg)

Synopsis
--------
The `mod_fsync` module for ProFTPD allows for configuring when the
`fsync(2)` system call is used to flush data to disk, for use on
kernels/filesystems where the buffer cache algorithms are less than desirable.

See the [mod_fsync.html](https://htmlpreview.github.io/?https://github.com/Castaglia/proftpd-mod_fsync/blob/master/mod_fsync.html) documentation for more
details.
