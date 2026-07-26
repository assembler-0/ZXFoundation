#!/usr/bin/env perl
# ZXFoundation configuration helper tool. Copyright (C) 2026 assembler-0.
# Licensed under the Apache License, Version 2.0.

use strict;
use warnings;
use File::Path qw(make_path);
use File::Basename qw(dirname);

my %DEFAULTS = (
    ZX_COMPILER_FAMILY  => 'clang',        # clang | gcc | unknown
    ZX_C_COMPILER       => 'clang',        # clang | gcc
    ZX_CXX_COMPILER     => 'clang++',      # clang | gcc
    ZX_COMPILER_VERSION => '',             # e.g. "17" -> clang-17 / clang++-17
    ZX_COMPILER_PREFIX  => '',             # cross-compiler prefix for gcc builds
    ZX_OPT_LEVEL        => '2',            # 0,1,2,3,s,z
    ZX_DSYM_LEVEL       => '0',            # 0,1,2,3
    ZX_MARCH_MODE       => 'z13',          # z13, z14, z15, z16 ...
    ZX_DASD_SERIAL      => '001842095440', # 12-digit DASD serial
    ZX_BUILD_DIR        => 'build',        # output directory
    ZX_TOOLCHAIN_FILE   => undef,          # default: $ZX_BUILD_DIR/toolchain.generated.cmake
    ZX_CONFIG_FILE      => undef,          # default: $ZX_BUILD_DIR/config.generated.cmake
    ZX_NOCHECK          => 'OFF',          # skip standard safeguard checks
);

my @VALID_COMPILERS = qw(clang gcc unknown);
my @VALID_OPT       = qw(0 1 2 3 s z);
my @VALID_DSYM      = qw(0 1 2 3);

my $CMAKE = $ENV{ZX_CMAKE_COMMAND} || 'cmake';

my %ALIASES = (
    family    => 'ZX_COMPILER_FAMILY',
    cc        => 'ZX_C_COMPILER',
    cxx       => 'ZX_CXX_COMPILER',
    version   => 'ZX_COMPILER_VERSION',
    prefix    => 'ZX_COMPILER_PREFIX',
    opt       => 'ZX_OPT_LEVEL',
    dsym      => 'ZX_DSYM_LEVEL',
    march     => 'ZX_MARCH_MODE',
    dasd      => 'ZX_DASD_SERIAL',
    dir       => 'ZX_BUILD_DIR',
    toolchain => 'ZX_TOOLCHAIN_FILE',
    config    => 'ZX_CONFIG_FILE',
    nocheck   => 'ZX_NOCHECK',
);

my $HELP_TEXT = <<'HELP';
ZXFoundation configuration helper tool. Copyright (C) 2026 assembler-0. Licensed under the Apache License, Version 2.0.

Usage:
    perl configure.pl [option=value ...] [command]

Options:
    family=     (*) clang | gcc | unknown                        (default: clang)
    cc=         (*) clang | gcc | unknown                        (default: clang)
    cxx=        (*) clang++ | g++ | unknown                      (default: clang++)
    version=        version suffix, e.g. 17                      (default: "")
    prefix=         cross prefix (for gcc)                       (default: "")
    opt=            0 | 1 | 2 | 3 | s | z                        (default: 2)
    dsym=           0 | 1 | 2 | 3                                (default: 0)
    march=          z13 | z14 | z15 | z16 ...                    (default: z13)
    dasd=           12-digit DASD serial                         (default: 001842095440)
    dir=            output directory                             (default: build)
    toolchain=      path for the generated toolchain file        (default: $dir/toolchain.generated.cmake)
    config=         path for the generated config file           (default: $dir/config.generated.cmake)
    nocheck=        on | off -- skip standard safeguard checks   (default: off)

Command:
    [help]      (X) shows this help                               (default)
    configure   (X) configures the project
    build       (X) configures and build the project
    genconfig   (X) generates configuration files

Notes:
    (*): Required option
    (X): Choose one of the options

Examples:
  perl configure.pl help
  perl configure.pl cc=clang cxx=clang++ build
  perl configure.pl family=gcc cc=gcc cxx=g++ prefix=s390x-linux-gnu version=12 build
HELP

sub parse_options {
    my @argv = @_;

    my %opts = map { $_ => $DEFAULTS{$_} } keys %DEFAULTS;
    my @commands;

    for my $arg (@argv) {
        if ($arg =~ /^([A-Za-z_][A-Za-z0-9_]*)=(.*)$/) {
            my ($key, $value) = (lc($1), $2);
            if (exists $ALIASES{$key}) {
                $opts{ $ALIASES{$key} } = $value;
            }
            else {
                die "configure.pl: unknown option '$key=' (see 'perl configure.pl help')\n";
            }
        }
        else {
            push @commands, $arg;
        }
    }

    if (@commands > 1) {
        die "configure.pl: only one command may be given (got: @commands)\n";
    }
    my $command = @commands ? $commands[0] : '';

    $opts{ZX_TOOLCHAIN_FILE} //= "$opts{ZX_BUILD_DIR}/toolchain.generated.cmake";
    $opts{ZX_CONFIG_FILE}    //= "$opts{ZX_BUILD_DIR}/config.generated.cmake";

    return (\%opts, $command);
}

sub is_truthy {
    my ($value) = @_;
    return 0 unless defined $value;
    return $value =~ /^(?:ON|YES|TRUE|Y|1)$/i ? 1 : 0;
}

sub validate_options {
    my ($opts) = @_;
    return if is_truthy($opts->{ZX_NOCHECK});

    unless (grep { $_ eq $opts->{ZX_COMPILER_FAMILY} } @VALID_COMPILERS) {
        die "ZX_COMPILER_FAMILY must be one of: @VALID_COMPILERS (got '$opts->{ZX_COMPILER_FAMILY}')\n";
    }
    unless (grep { $_ eq $opts->{ZX_OPT_LEVEL} } @VALID_OPT) {
        die "ZX_OPT_LEVEL must be one of: @VALID_OPT (got '$opts->{ZX_OPT_LEVEL}')\n";
    }
    unless (grep { $_ eq $opts->{ZX_DSYM_LEVEL} } @VALID_DSYM) {
        die "ZX_DSYM_LEVEL must be one of: @VALID_DSYM (got '$opts->{ZX_DSYM_LEVEL}')\n";
    }
    unless ($opts->{ZX_DASD_SERIAL} =~ /^[0-9]+$/) {
        die "ZX_DASD_SERIAL must be numeric (got '$opts->{ZX_DASD_SERIAL}')\n";
    }
    return;
}

sub resolve_compiler_names {
    my ($opts) = @_;
    my $version = $opts->{ZX_COMPILER_VERSION};
    my $prefix  = $opts->{ZX_COMPILER_PREFIX};

    my $cc  = $opts->{ZX_C_COMPILER};
    my $cxx = $opts->{ZX_CXX_COMPILER};

    $cc  .= "-$version" if $version ne '';
    $cxx .= "-$version" if $version ne '';
    $cc   = "$prefix-$cc"  if $prefix ne '';
    $cxx  = "$prefix-$cxx" if $prefix ne '';

    return ($cc, $cxx);
}

sub build_toolchain_content {
    my ($opts, $cc, $cxx) = @_;

    my $tc = "# Autogenerated by configure.pl -- do not edit by hand\n";
    $tc .= "# ZXFoundation $opts->{ZX_C_COMPILER}/$opts->{ZX_CXX_COMPILER} toolchain for s390x\n\n";
    $tc .= "set(CMAKE_SYSTEM_NAME Generic)\n";
    $tc .= "set(CMAKE_SYSTEM_PROCESSOR s390x)\n";
    $tc .= "set(CMAKE_C_COMPILER $cc)\n";
    $tc .= "set(CMAKE_CXX_COMPILER $cxx)\n";
    $tc .= "set(CMAKE_ASM_COMPILER $cc)\n\n";

    if ($opts->{ZX_COMPILER_FAMILY} eq 'clang') {
        $tc .= "set(CMAKE_LINKER ld.lld)\n";
        $tc .= "set(CMAKE_AR llvm-ar)\n";
        $tc .= "set(CMAKE_RANLIB llvm-ranlib)\n";
        $tc .= "set(CMAKE_OBJCOPY llvm-objcopy)\n";
        $tc .= "set(CMAKE_OBJDUMP llvm-objdump)\n";
        $tc .= "set(ZX_NM llvm-nm)\n";
        $tc .= "set(ZX_CXXFILT llvm-cxxfilt)\n";
        $tc .= "set(ZX_HOST_CC clang)\n\n";
        $tc .= "set(COMPILER_ID \"clang\")\n";
    }
    else {
        # gcc and unknown
        my $prefix = $opts->{ZX_COMPILER_PREFIX};
        if ($prefix ne '') {
            $tc .= "set(CMAKE_LINKER $prefix-ld)\n";
            $tc .= "set(CMAKE_AR $prefix-ar)\n";
            $tc .= "set(CMAKE_RANLIB $prefix-ranlib)\n";
            $tc .= "set(CMAKE_OBJCOPY $prefix-objcopy)\n";
            $tc .= "set(CMAKE_OBJDUMP $prefix-objdump)\n";
            $tc .= "set(ZX_NM $prefix-nm)\n";
            $tc .= "set(ZX_CXXFILT $prefix-c++filt)\n";
        }
        else {
            $tc .= "set(CMAKE_LINKER ld)\n";
            $tc .= "set(CMAKE_AR ar)\n";
            $tc .= "set(CMAKE_RANLIB ranlib)\n";
            $tc .= "set(CMAKE_OBJCOPY objcopy)\n";
            $tc .= "set(CMAKE_OBJDUMP objdump)\n";
            $tc .= "set(ZX_NM nm)\n";
            $tc .= "set(ZX_CXXFILT c++filt)\n";
        }
        $tc .= "set(ZX_HOST_CC $opts->{ZX_C_COMPILER})\n\n";
        $tc .= "set(COMPILER_ID \"$opts->{ZX_COMPILER_FAMILY}\")\n";
    }

    return $tc;
}

sub build_config_content {
    my ($opts) = @_;

    my $cfg = "# Autogenerated by configure.pl -- do not edit by hand\n";
    $cfg .= "# Standard configuration\n\n";
    $cfg .= "set(OPT_LEVEL \"$opts->{ZX_OPT_LEVEL}\" CACHE STRING \"Optimization level (0, 1, 2, 3, s, z)\")\n";
    $cfg .= "set(DSYM_LEVEL \"$opts->{ZX_DSYM_LEVEL}\" CACHE STRING \"Debug symbol level (0, 1, 2, 3)\")\n";
    $cfg .= "set(MARCH_MODE \"$opts->{ZX_MARCH_MODE}\" CACHE STRING \"Architecture mode, for -march and -mtune\")\n";
    $cfg .= "set(DASD_SERIAL \"$opts->{ZX_DASD_SERIAL}\" CACHE STRING \"Dasd serial\")\n";

    return $cfg;
}

sub write_generated_files {
    my ($opts, $toolchain_content, $config_content) = @_;

    for my $path ($opts->{ZX_TOOLCHAIN_FILE}, $opts->{ZX_CONFIG_FILE}) {
        my $dir = dirname($path);
        make_path($dir) if $dir && $dir ne '.' && !-d $dir;
    }

    open(my $tc_fh, '>', $opts->{ZX_TOOLCHAIN_FILE})
        or die "configure.pl: cannot write '$opts->{ZX_TOOLCHAIN_FILE}': $!\n";
    print {$tc_fh} $toolchain_content;
    close $tc_fh;

    open(my $cfg_fh, '>', $opts->{ZX_CONFIG_FILE})
        or die "configure.pl: cannot write '$opts->{ZX_CONFIG_FILE}': $!\n";
    print {$cfg_fh} $config_content;
    close $cfg_fh;

    return;
}

sub run_cmake_configure {
    my ($opts) = @_;
    my @cmd = (
        $CMAKE, '-B', $opts->{ZX_BUILD_DIR},
        "-DZX_TOOLCHAIN_FILE=$opts->{ZX_TOOLCHAIN_FILE}",
        "-DZX_CONFIG_FILE=$opts->{ZX_CONFIG_FILE}",
        '-GNinja',
    );
    system(@cmd);
    return $? == -1 ? 1 : ($? >> 8);
}

sub run_cmake_build {
    my ($opts) = @_;
    my @cmd = ($CMAKE, '--build', $opts->{ZX_BUILD_DIR});
    system(@cmd);
    return $? == -1 ? 1 : ($? >> 8);
}

sub print_summary {
    my ($opts, $command, $cc, $cxx) = @_;
    $cc  //= '';
    $cxx //= '';

    print " ZXFoundation configure.pl configuration overview:\n";
    print "  Command         : $command\n";
    print "  Compiler family : $opts->{ZX_COMPILER_FAMILY} ($cc/$cxx)\n";
    print "  Optimization    : $opts->{ZX_OPT_LEVEL}\n";
    print "  Debug symbols   : $opts->{ZX_DSYM_LEVEL}\n";
    print "  Architecture    : $opts->{ZX_MARCH_MODE}\n";
    print "  DASD serial     : $opts->{ZX_DASD_SERIAL}\n";
    print "  Generated       : $opts->{ZX_TOOLCHAIN_FILE}\n";
    print "                    $opts->{ZX_CONFIG_FILE}\n";
    return;
}

sub main {
    my ($opts, $command) = parse_options(@_);

    if ($command eq '' || $command eq 'help') {
        print $HELP_TEXT;
        return 0;
    }

    validate_options($opts);

    my ($cc, $cxx);
    my $configure_result;

    if ($command eq 'configure' || $command eq 'build' || $command eq 'genconfig') {
        ($cc, $cxx) = resolve_compiler_names($opts);
        my $toolchain_content = build_toolchain_content($opts, $cc, $cxx);
        my $config_content    = build_config_content($opts);
        write_generated_files($opts, $toolchain_content, $config_content);
    }

    if ($command eq 'configure' || $command eq 'build') {
        $configure_result = run_cmake_configure($opts);
    }

    if ($command eq 'configure' || $command eq 'build' || $command eq 'genconfig') {
        print_summary($opts, $command, $cc, $cxx);
    }
    else {
        die "configure.pl: unknown command: $command.\n";
    }

    if ($command eq 'genconfig') {
        print "Next steps:\n";
        print "  $CMAKE -B $opts->{ZX_BUILD_DIR} -DZX_TOOLCHAIN_FILE=$opts->{ZX_TOOLCHAIN_FILE} -DZX_CONFIG_FILE=$opts->{ZX_CONFIG_FILE} -GNinja\n";
        print "  $CMAKE --build $opts->{ZX_BUILD_DIR}\n";
    }
    elsif ($command eq 'configure') {
        if ($configure_result != 0) {
            print "WARNING: configure may have failed. please recheck your configuration.\n";
            print "configure.pl: nothing to do. exiting\n";
            return 0;
        }
        else {
            print "Next steps:\n";
            print "  $CMAKE --build $opts->{ZX_BUILD_DIR}\n";
        }
    }
    elsif ($command eq 'build') {
        if ($configure_result != 0) {
            print "WARNING: configure may have failed. please recheck your configuration.\n";
            print "configure.pl: nothing to do. exiting\n";
            return 0;
        }

        my $build_result = run_cmake_build($opts);
        if ($build_result != 0) {
            print "WARNING: build may have failed. please recheck your configuration.\n";
            return 0;
        }
    }

    print "configure.pl: nothing to do. exiting\n";
    return 0;
}

exit main(@ARGV);