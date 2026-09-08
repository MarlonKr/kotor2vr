"""CPU-only tests of the bounded-age policy, before AND after patch adoption.

Use an already patched source directly; otherwise apply to a temporary copy.
Compile the actual policy and trace helpers with MSVC, including JSON export.
No active source edits, CMake, D3D device, worker process, or game launch.
"""
from pathlib import Path
import subprocess
import json
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path("src/host64/src/neural_stereo_pipeline.cpp")
PATCH = ROOT / "artifacts/age-bounded-prefetch.patch"
VCVARS = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat")
BEGIN = "// BEGIN bounded-age prefetch policy (CPU-tested from the unapplied patch)."
END = "// END bounded-age prefetch policy."

MAIN = r'''
#include <cassert>
#include <cstdio>
int main(int argc,char** argv) {
    assert(argc==2);
    const wchar_t* invalid[]={nullptr,L"",L"0",L"0.0",L"-0",L"-4",L"nan",L"NaN",L"inf",L"+inf",
        L"infinity",L"1e9999",L"1e-9999",L" 4",L"4 ",L"4ms",L"4,5",L"0x1p2",L"+",L"++4",L"4e",L"\uFF14"};
    for (const auto* text:invalid) assert(ParsePrefetchMaxAgeMs(true,text)==0.0);
    assert(ParsePrefetchMaxAgeMs(true,L"4")==4.0);
    assert(ParsePrefetchMaxAgeMs(true,L"8")==8.0);
    assert(ParsePrefetchMaxAgeMs(true,L"+4.5")==4.5);
    assert(ParsePrefetchMaxAgeMs(true,L"4e0")==4.0);
    assert(ParsePrefetchMaxAgeMs(true,L".5")==0.5);
    assert(ParsePrefetchMaxAgeMs(true,L"1e-20")>0.0);
    assert(ParsePrefetchMaxAgeMs(false,L"4")==0.0); // Limit never enables prefetch.
    assert(ParsePrefetchMaxAgeMs(false,L"8")==0.0);
    const std::wstring oversized(100,L'1');
    assert(ParsePrefetchMaxAgeMs(true,oversized.c_str())==0.0);

    using Clock=PrefetchAgePolicy::Clock;
    static_assert(Clock::is_steady);
    bool reset=false;
    const auto admitted=Clock::time_point{}+std::chrono::seconds(100);
    for (const unsigned limit:{4U,8U}) {
        PrefetchAgePolicy gate; gate.max_age_ms=limit;
        const auto boundary=admitted+std::chrono::milliseconds(limit);
        std::uint32_t reasons=0;
        assert(gate.TryBegin(admitted,boundary-std::chrono::microseconds(1),reasons,reset));
        assert(gate.TryBegin(admitted,boundary,reasons,reset)); // Exactly equal is allowed.
        reasons=1U<<5;
        assert(!gate.TryBegin(admitted,boundary+std::chrono::microseconds(1),reasons,reset));
        assert(gate.dropped_inputs==1 && gate.pending_reset_reasons==(1U<<5));
        reasons=1U<<4;
        assert(gate.TryBegin(boundary,boundary+std::chrono::microseconds(1),reasons,reset));
        assert(reasons==((1U<<5)|(1U<<4)) && gate.pending_reset_reasons==0);
        reasons=0;
        assert(gate.TryBegin(boundary,boundary+std::chrono::microseconds(2),reasons,reset));
        assert(reasons==0); // Deferred resets consumed exactly once.
    }

    PrefetchAgePolicy delayed; delayed.max_age_ms=4;
    std::uint32_t reasons=1;
    // Submission could have occurred at 3.99 ms; admission is still the origin.
    assert(!delayed.TryBegin(admitted,admitted+std::chrono::microseconds(4001),reasons,reset));
    // A newer queued replacement has a new admission timestamp and may start.
    const auto replacement_time=admitted+std::chrono::microseconds(3999);
    reasons=0;
    assert(delayed.TryBegin(replacement_time,admitted+std::chrono::microseconds(4001),reasons,reset));
    assert(reasons==1);

    PrefetchAgePolicy resets; resets.max_age_ms=4;
    reasons=PrefetchAgePolicy::AdmissionResetReasons(false,0xFFFFFFFFU,1U<<4);
    assert(reasons==(1U<<4)); // An Empty slot's previous frame must not leak resets.
    reasons=PrefetchAgePolicy::AdmissionResetReasons(true,reasons,1U<<5);
    reasons=PrefetchAgePolicy::AdmissionResetReasons(true,reasons,0);
    assert(reasons==((1U<<4)|(1U<<5))); // Several Queued replacements preserve bits.
    assert(!resets.TryBegin(admitted,admitted+std::chrono::milliseconds(5),reasons,reset));
    reasons=PrefetchAgePolicy::AdmissionResetReasons(false,reasons,1U<<9);
    assert(!resets.TryBegin(admitted,admitted+std::chrono::milliseconds(6),reasons,reset));
    assert(resets.pending_reset_reasons==((1U<<4)|(1U<<5)|(1U<<9)));
    reasons=1U<<10;
    assert(resets.TryBegin(admitted,admitted,reasons,reset));
    assert(reasons==((1U<<4)|(1U<<5)|(1U<<9)|(1U<<10)));
    assert(resets.pending_reset_reasons==0 && resets.dropped_inputs==2);

    PrefetchAgePolicy off; off.max_age_ms=ParsePrefetchMaxAgeMs(true,L"0");
    reasons=0;
    assert(!off.enabled());
    assert(off.TryBegin(admitted,admitted+std::chrono::hours(24),reasons,reset));
    assert(off.dropped_inputs==0);

    PrefetchAgePolicy sustained; sustained.max_age_ms=8;
    for (unsigned i=0;i<5000;++i) {
        reasons=1U<<(i%11);
        assert(!sustained.TryBegin(admitted,admitted+std::chrono::milliseconds(9),reasons,reset));
    }
    assert(sustained.dropped_inputs==5000 && sustained.pending_reset_reasons==0x7FF);
    reasons=0;
    assert(sustained.TryBegin(admitted,admitted,reasons,reset) && reasons==0x7FF);
    for (unsigned i=0;i<1000;++i) {
        reasons=0;
        assert(sustained.TryBegin(admitted,admitted,reasons,reset) && reasons==0);
    }
    assert(sustained.dropped_inputs==5000); // No dependence on trace/sample limits.
    PrefetchAgePolicy boolean_only; boolean_only.max_age_ms=4;
    reasons=0; reset=true;
    assert(!boolean_only.TryBegin(admitted,admitted+std::chrono::milliseconds(5),reasons,reset));
    assert(boolean_only.pending_reset && boolean_only.pending_reset_reasons==0);
    reasons=0; reset=false;
    assert(!boolean_only.TryBegin(admitted,admitted+std::chrono::milliseconds(6),reasons,reset));
    assert(boolean_only.pending_reset); // A reset-free drop must not clear the pending bool.
    reasons=0; reset=false;
    assert(boolean_only.TryBegin(admitted,admitted,reasons,reset));
    assert(reset && reasons==0 && !boolean_only.pending_reset);
    reset=false;
    assert(boolean_only.TryBegin(admitted,admitted,reasons,reset) && !reset);

    NeuralTimingTrace trace;
    trace.size=NeuralTimingTrace::limit;
    for (auto& row:trace.pairs) row.discarded_age=true;
    assert(trace.ReadyForSnapshot()); // 360 age drops must settle without worker stamps.
    auto& worked=trace.pairs[0]; worked.discarded_age=false;
    assert(!trace.ReadyForSnapshot());
    worked.work_published=true;
    assert(!trace.ReadyForSnapshot());
    worked.display_submit_qpc=1;
    assert(trace.ReadyForSnapshot());
    auto& replaced=trace.pairs[1]; replaced.discarded_age=false; replaced.replaced_input=true;
    assert(trace.ReadyForSnapshot());
    auto& superseded=trace.pairs[2]; superseded.discarded_age=false; superseded.superseded_output=true;
    assert(!trace.ReadyForSnapshot());
    superseded.work_published=true;
    assert(trace.ReadyForSnapshot());
    trace.path=argv[1]; trace.age_dropped_inputs=5000; trace.prefetch_max_age_ms=8;
    trace.snapshot_qpc=1; trace.Write(2040,2232,2040,2232,true,false);
    puts("PASS: bool-only reset; 360 age drops settle; active/unpublished rows block; trace total+row export");
    puts("PASS: strict env/off; 4/8-ms boundaries; admission age; fresh replacement; reset OR/consume-once; 5000 drops + 1000 accepts");
}
'''


def prepare_source(baseline, folder):
    decoded = baseline.decode("utf-8").replace("\r\n", "\n")
    if BEGIN in decoded:
        return decoded, "applied source"  # Never apply the patch a second time.
    else:
        staged = folder / SOURCE
        staged.parent.mkdir(parents=True)
        staged.write_bytes(baseline)
        # git apply operates only on this disposable snapshot, never the live file.
        subprocess.run(["git", "apply", "--check", str(PATCH)], cwd=folder, check=True)
        subprocess.run(["git", "apply", str(PATCH)], cwd=folder, check=True)
        return staged.read_text(encoding="utf-8"), "unapplied patch on temporary copy"


def main():
    baseline = (ROOT / SOURCE).read_bytes()
    with tempfile.TemporaryDirectory(prefix="kotor-prefetch-age-cpu-") as temp:
        folder = Path(temp)
        changed, mode = prepare_source(baseline, folder)
        # Exercise adoption detection even when the active source is unpatched.
        adopted, adopted_mode = prepare_source(changed.encode("utf-8"), folder / "must-not-be-created")
        assert adopted == changed and adopted_mode == "applied source"
        assert not (folder / "must-not-be-created").exists()
        assert BEGIN in changed and END in changed
        assert "candidate.state==InputState::Queued" in changed
        assert "TryBegin(candidate.admitted_at,Clock::now(),candidate.reset_reasons,candidate.reset)" in changed
        record = changed.split("bool NeuralStereoPipeline::RecordInput(", 1)[1].split("void NeuralStereoPipeline::SubmitRecordedInput(", 1)[0]
        assert "input.admitted_at=Impl::Clock::now()" in record
        assert "AdmissionResetReasons(replaced,input.reset_reasons,frame.request.history_reset_reasons)" in record
        # The original submit/fence/output-retirement contract is byte-for-byte unchanged.
        marker = "void NeuralStereoPipeline::SubmitRecordedInput("
        assert changed.split(marker, 1)[1] == baseline.decode("utf-8").replace("\r\n", "\n").split(marker, 1)[1]

        source = folder / "age_cpu.cpp"
        prefix, boundary, _ = changed.partition("void Check(HRESULT result,const char* stage)")
        assert boundary, "private CPU helper boundary changed"
        source.write_text(prefix + "\n} } // production helper namespaces\nusing namespace kotorvr::host;\n" + MAIN, encoding="utf-8")
        executable = folder / "age_cpu.exe"
        flags = ["cl.exe", "/nologo", "/std:c++20", "/EHsc", "/W4", "/WX",
                 "/DNOMINMAX", "/DWIN32_LEAN_AND_MEAN", "/I", str(ROOT / "src/host64/include"),
                 "/I", str(ROOT / "src/common"), str(source), "/Fe:" + str(executable)]
        command = f'"{VCVARS}" >nul && ' + subprocess.list2cmdline(flags)
        subprocess.run(f'cmd.exe /d /s /c "{command}"', cwd=folder, check=True)
        output = folder / "trace.jsonl"
        subprocess.run([str(executable), str(output)], cwd=folder, check=True)
        session, *rows = [json.loads(line) for line in output.read_text().splitlines()]
        assert session["age_dropped_inputs"] == 5000 and session["prefetch_max_age_ms"] == 8
        assert session["export_reason"] == "settled_limit" and session["counters_scope"] == "through_snapshot"
        assert len(rows) == 360 and sum(row["discarded_age"] for row in rows) == 357
        assert rows[1]["replaced_input"] and not rows[1]["discarded_age"]
        assert all(not row["work_start_qpc"] for row in rows if row["discarded_age"])
    print("PASS:", mode, "; applied-source detection; SubmitRecordedInput/fence contract unchanged")


if __name__ == "__main__":
    main()
