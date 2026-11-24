#!/usr/bin/env python3
"""Resume um trace do Perfetto do jogo (scripts/android_trace.py): onde cada thread passa o tempo.

Para cada thread do processo do jogo: % do tempo rodando, na fila sem núcleo (Runnable: o
escalonador não deu CPU), dormindo (S: esperando algo) e em D; o clock médio dos núcleos
enquanto ela rodou e em que tipo de núcleo; quem a acorda (as threads que mais a tiraram do
sono). E o clock da GPU e dos clusters no período. Serve para separar "thread no limite",
"thread esperando outra" (serialização) e "thread pronta sem núcleo/clock baixo" (escalonador).

    python scripts/android_trace_report.py work/android/a54_dxvk_trace.pftrace [--top 14]
"""
import argparse
import sys

from perfetto.trace_processor import TraceProcessor

PROC_LIKE = "com.re5.recompiled:game"


def q(tp, sql):
    return list(tp.query(sql))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("trace")
    ap.add_argument("--top", type=int, default=14)
    ap.add_argument("--process", default=PROC_LIKE)
    args = ap.parse_args()
    tp = TraceProcessor(trace=args.trace)

    b = q(tp, "select trace_start() as s, trace_end() as e")[0]
    t0, t1 = b.s, b.e
    dur = (t1 - t0) / 1e9
    procs = q(tp, f"select upid, pid, name from process where name like '{args.process}%'")
    if not procs:
        sys.exit(f"[!] processo {args.process} não está no trace")
    upid = procs[0].upid
    print(f"trace {dur:.1f} s; processo {procs[0].name} (pid {procs[0].pid})")

    # núcleos: máximo de cada um (para dizer grande/pequeno e o clock relativo)
    fmax = {}
    for r in q(tp, """select cast(t.cpu as int) as cpu, max(c.value) as v
                      from counter c join cpu_counter_track t on c.track_id = t.id
                      where t.name = 'cpufreq' group by t.cpu"""):
        fmax[r.cpu] = r.v
    top_f = max(fmax.values()) if fmax else 0
    big = {c for c, f in fmax.items() if f >= 0.9 * top_f}

    # clock médio ponderado no tempo, por núcleo
    fav = {}
    for r in q(tp, f"""select cpu, sum(v * d) / sum(d) as v from (
                         select cast(t.cpu as int) as cpu, c.value as v,
                                coalesce(lead(c.ts) over (partition by t.cpu order by c.ts), {t1}) - c.ts as d
                         from counter c join cpu_counter_track t on c.track_id = t.id
                         where t.name = 'cpufreq') group by cpu"""):
        fav[r.cpu] = r.v
    if fav:
        print("clock médio dos núcleos (MHz / máx): " +
              " ".join(f"cpu{c}={fav[c] / 1000:.0f}/{fmax.get(c, 0) / 1000:.0f}{'G' if c in big else 'p'}"
                       for c in sorted(fav)))

    g = q(tp, f"""select c.value as v, c.ts as ts from counter c join gpu_counter_track t on c.track_id = t.id
                  where t.name like '%gpufreq%' or t.name like '%gpu_freq%' order by c.ts""")
    if g:
        tot = 0.0
        acc = 0.0
        for i, r in enumerate(g):
            end = g[i + 1].ts if i + 1 < len(g) else t1
            d = max(0, end - max(r.ts, t0))
            acc += r.v * d
            tot += d
        vals = sorted({int(r.v) for r in g})
        unit = 1e6 if vals[-1] > 1e8 else (1e3 if vals[-1] > 1e5 else 1)
        print(f"GPU: clock médio {acc / tot / unit:.0f} MHz; valores vistos (MHz): "
              + ", ".join(f"{v / unit:.0f}" for v in vals[:12]) + f"; {len(g)} trocas")
    else:
        print("GPU: sem eventos de clock no trace")

    # estados por thread
    rows = q(tp, f"""
        select t.utid, t.tid, t.name,
          sum(case when s.state = 'Running' then s.dur else 0 end) as run,
          sum(case when s.state in ('R', 'R+') then s.dur else 0 end) as rnbl,
          sum(case when s.state = 'S' then s.dur else 0 end) as sleep,
          sum(case when s.state like 'D%' then s.dur else 0 end) as dstate
        from thread t join thread_state s using (utid)
        where t.upid = {upid}
        group by t.utid order by run desc""")
    tot_ns = (t1 - t0)
    print(f"\nthreads (% dos {dur:.1f} s: rodando / pronta sem núcleo / dormindo / D; clock médio enquanto rodou; "
          "% do tempo rodando em núcleo grande):")
    top = rows[:args.top]
    for r in top:
        # clock médio enquanto rodou: média do clock do núcleo no início de cada fatia (aproximação)
        f = q(tp, f"""
            select avg((select c.value from counter c join cpu_counter_track t on c.track_id = t.id
                        where t.name = 'cpufreq' and t.cpu = s.cpu and c.ts <= s.ts order by c.ts desc limit 1)) as f,
                   sum(case when s.cpu in ({','.join(str(c) for c in big) or '-1'}) then s.dur else 0 end) * 1.0
                     / max(sum(s.dur), 1) as bigfrac
            from sched_slice s where s.utid = {r.utid}""")[0]
        fmhz = f"{f.f / 1000:.0f} MHz" if f.f else "?"
        print(f"  {r.tid:6d} {r.name or '?':18s} {100 * r.run / tot_ns:5.1f}% {100 * r.rnbl / tot_ns:5.1f}% "
              f"{100 * r.sleep / tot_ns:5.1f}% {100 * r.dstate / tot_ns:4.1f}%   {fmhz:>9s}  "
              f"grande {100 * (f.bigfrac or 0):3.0f}%")

    # quem acorda as threads principais
    print("\nquem acorda (wakeups no trace; o waker é a thread que chamou o wakeup):")
    for r in top[:6]:
        w = q(tp, f"""
            select coalesce(wt.name, '?') as name, wt.tid as tid, count(*) as n
            from thread_state s left join thread wt on s.waker_utid = wt.utid
            where s.utid = {r.utid} and s.state in ('R', 'R+') and s.waker_utid is not null
            group by s.waker_utid order by n desc limit 5""")
        if w:
            print(f"  {r.name}: " + ", ".join(f"{x.name}({x.tid}) {x.n}" for x in w))

    # sono: duração típica dos sonos das principais (esperas longas = esperando outra thread/GPU)
    print("\nsonos (S) das threads principais: quantos, mediana e p90 em ms:")
    for r in top[:6]:
        d = [x.dur for x in q(tp, f"select dur from thread_state where utid = {r.utid} and state = 'S' order by dur")]
        if d:
            print(f"  {r.name:18s} {len(d):6d} sonos, mediana {d[len(d) // 2] / 1e6:6.2f} ms, "
                  f"p90 {d[int(len(d) * 0.9)] / 1e6:6.2f} ms, soma {sum(d) / 1e9:5.2f} s")


if __name__ == "__main__":
    main()
