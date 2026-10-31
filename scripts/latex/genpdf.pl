#!/usr/bin/env perl
use strict;
use warnings;
use utf8;

use Cwd            qw(getcwd chdir);
use File::Spec     qw();
use Time::HiRes    qw(gettimeofday tv_interval);
use IPC::Cmd       qw(can_run);
use Term::ANSIColor qw(:constants);

my $DOXYFILE    = 'Doxyfile.pdf';
my $BUILD_DIR   = File::Spec->catdir('doxygen-build', 'latex');
my $TARGET_BASE = 'refman';
my $MAX_PASSES  = 3;

sub log_info { print CYAN,  "[info] ", RESET, @_, "\n"; }
sub log_ok   { print GREEN, "[ ok ] ", RESET, @_, "\n"; }
sub log_warn { print YELLOW,"[warn] ", RESET, @_, "\n"; }
sub log_err  { print RED,   "[fail] ", RESET, @_, "\n"; }

sub run_command {
    my ($cmd_ref, $step_desc) = @_;
    my @cmd = @$cmd_ref;

    print BOLD, "==> $step_desc...\n", RESET;
    my $t0 = [gettimeofday];

    my $exit_code = system(@cmd);

    my $elapsed = sprintf("%.2fs", tv_interval($t0));

    if ($exit_code != 0) {
        log_err("Command failed after $elapsed: @cmd");
        return 0;
    }

    log_ok("$step_desc completed in $elapsed");
    return 1;
}

log_info("Checking required tools...");
my @required_tools = qw(doxygen xelatex makeindex);

for my $tool (@required_tools) {
    my $bin = can_run($tool);
    if (!defined $bin) {
        log_err("Missing required binary: '$tool'. Please install it and ensure it's in your PATH.");
        exit 1;
    }
    log_ok("Found $tool: $bin");
}

if (! -f $DOXYFILE) {
    log_err("Doxyfile configuration not found: '$DOXYFILE'");
    exit 1;
}

my $overall_t0 = [gettimeofday];

log_info("Starting Doxygen generation...");
if (!run_command(['doxygen', $DOXYFILE], "Running Doxygen ($DOXYFILE)")) {
    exit 1;
}

if (! -d $BUILD_DIR) {
    log_err("Expected build directory does not exist: '$BUILD_DIR'");
    exit 1;
}

my $orig_dir = getcwd();
chdir($BUILD_DIR) or die "Cannot change directory to $BUILD_DIR: $!\n";
log_info("Changed working directory to: $BUILD_DIR");

$ENV{max_print_line} = 2000;

for my $i (1 .. $MAX_PASSES) {
    my $step_name = "XeLaTeX (Pass $i of $MAX_PASSES)";

    my $success = run_command([
        'xelatex',
        '-interaction=nonstopmode',
        $TARGET_BASE
    ], $step_name);

    if (!defined $success) {
        log_err("LaTeX compilation aborted on pass $i.");
        chdir($orig_dir);
        exit 1;
    }

    # Generate index after pass 1
    if ($i == 1) {
        my $idx_file = "$TARGET_BASE.idx";
        if (-f $idx_file) {
            if (!run_command(['makeindex', $idx_file], "Generating Index ($idx_file)")) {
                log_warn("Makeindex encountered issues, but continuing...");
            }
        } else {
            log_warn("No '$idx_file' found; skipping makeindex.");
        }
    }
}

chdir($orig_dir);

my $pdf_path = File::Spec->catfile($BUILD_DIR, "$TARGET_BASE.pdf");
my $total_elapsed = sprintf("%.2fs", tv_interval($overall_t0));

print "\n" . "=" x 50 . "\n";
if (-f $pdf_path) {
    my $size_kb = sprintf("%.2f MB", (-s $pdf_path) / (1024 * 1024));
    log_ok(BOLD . "PDF Compilation Succeeded" . RESET);
    log_info("Output File: $pdf_path ($size_kb)");
    log_info("Total Time:  $total_elapsed");
} else {
    log_err("Build finished, but '$pdf_path' was not generated.");
    exit 1;
}
print "=" x 50 . "\n";