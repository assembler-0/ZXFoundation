# Patching hercules

--- 

## What it does

- Fix SMP cap of 128 vCPUs
- Add support for DIAG X'318'
- Add support for DIAG X'288'

## The patching part

Patching Hercules is surprisingly trivial. All the patches are included in `scripts/patches`

## After patching

Now you would want to reconfigure/rebuild and reinstall Hercules. 
For patching smp, you may want to specify CPU cap by `--enable-multi-cpu=N` where N is your desired number of emulated cpus
