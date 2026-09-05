# Probe: instancing (cards / archives / native instances) — usdGen plan appendix

Environment: OpenUSD 26.08 install at /home/burkard/work/OpenUSD_26_08 (headless, no GL).

    python3 makeStage.py probe.usda            # writes probe.usda + probe_scalp.usda
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C build
    ./build/instProbe  probe.usda   # A: what a downstream SI sees; B: synthesized
                                    #    instancer; C: emulation round-trip
    ./build/instProbe2 probe.usda   # generator inserted UPSTREAM of pi/ni propagation
    ./build/instProbe3 probe.usda   # nested instancer synthesized DOWNSTREAM
    ./build/instProbe4 probe.usda   # picking round-trip (HdxPrimOriginInfo)

Captured outputs: out-probe1.txt .. out-probe4.txt
