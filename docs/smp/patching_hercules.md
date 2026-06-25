# Patching hercules

--- 

## Notes

As you know, Hercules (or recently, Hyperion) only supports up to 128 vCPUs at compile time, and is a hardcoded limit.
With the addition if `_BitInt(N)` introduced in standardized C23, this restriction can finally be lifted. This is useful
because the ZXFoundation kernel can support up to 768 CPUs. Take it at your own risk, i am **NOT** responsible for any
physical, software damage it may cause.

## The patching part

Patching Hercules is surprisingly trivial. The `script/` directory included `hercules-xtra-smp.patch` that you just need to apply.

## Reconfiguring and building

The patch modifies `configure.ac` so it is a good idea to reconfigure project. An example:
```shell
autoreconf -fi
```

Now, you would want to configure the project. By default, the system utilize a 128 vCPUs configuration. You can set your desired number of
supported vCPUs by using the flag:
```text
--enable-multi-cpu=N
```

Where `N` is your desired number of supported vCPUs and is larger or equal to 1  
