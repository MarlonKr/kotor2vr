"""CPU-only trace checks; no CMake, D3D device, worker or game launch.

Run with Python on Windows with VS 2022 BuildTools installed. Compiles the
actual private trace helper prefix from neural_stereo_pipeline.cpp in a temp
directory, so neither GPU implementation nor copied serialization is tested.
"""
import json
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
VCVARS = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat")
SOURCE = ROOT / "src/host64/src/neural_stereo_pipeline.cpp"
MAIN = r'''
} } // close production helper namespaces
#include <cassert>
int main(int argc,char** argv) {
    using namespace kotorvr::host;
    assert(argc==2);
    const auto path=std::filesystem::absolute(argv[1]);
    SetEnvironmentVariableW(L"KOTOR2VR_NEURAL_TIMING_TRACE",nullptr);
    assert(!NeuralTimingTrace::Create());
    SetEnvironmentVariableW(L"KOTOR2VR_NEURAL_TIMING_TRACE",L"0");
    assert(!NeuralTimingTrace::Create());
    SetEnvironmentVariableW(L"KOTOR2VR_NEURAL_TIMING_TRACE",L"relative.jsonl");
    assert(!NeuralTimingTrace::Create());
    SetEnvironmentVariableW(L"KOTOR2VR_NEURAL_TIMING_TRACE",L"1");
    SetEnvironmentVariableW(L"KOTOR2VR_NEURAL_TIMING_TRACE_PATH",nullptr);
    auto automatic=NeuralTimingTrace::Create();
    assert(automatic && automatic->path.is_absolute());
    SetEnvironmentVariableW(L"KOTOR2VR_NEURAL_TIMING_TRACE_PATH",path.c_str());
    auto legacy=NeuralTimingTrace::Create();
    assert(legacy && legacy->path==path);
    // A direct path must override an unrelated legacy PATH setting.
    SetEnvironmentVariableW(L"KOTOR2VR_NEURAL_TIMING_TRACE_PATH",L"ignored-relative.jsonl");
    SetEnvironmentVariableW(L"KOTOR2VR_NEURAL_TIMING_TRACE",path.c_str());
    auto trace=NeuralTimingTrace::Create();
    assert(trace && trace->path==path && trace->frequency>0);
    k2vr::ipc::StereoFrameMetadata frame{};
    frame.camera_frame_id=87; frame.request.frame_id=91;
    frame.request.header.generation=3; frame.request.predicted_display_time_ns=123456789;
    NeuralTracePair* first=nullptr;
    for (unsigned i=0;i<365;++i) {
        frame.ready_value=100+i;
        // Legacy omission, explicit zero, old source, and future source.
        auto* p=i%4==0 ? trace->Admit(frame,TraceQpc(),i%2!=0) :
            trace->Admit(frame,TraceQpc(),i%2!=0,
                i%4==1 ? 0 : frame.request.predicted_display_time_ns+(i%4==2 ? 30000000LL:-7000000LL));
        if (i>=360) { assert(!p); continue; }
        assert(p && p->id==i+1 && p->admission_qpc>=p->request_qpc);
        if (!i) first=p;
    }
    assert(trace->size==360 && trace->admissions==365);
    assert(first==&trace->pairs[0] && first->ready_value==100);
    first->eye_pid[0]=111; first->eye_pid[1]=222;
    first->eye_sequence[0]=17; first->eye_sequence[1]=17;
    first->replaced_input=true;
    trace->pairs[1].superseded_output=true;
    // Exercise the production selection marker, including omitted/explicit zero.
    for (unsigned i=2;i<6;++i) {
        auto& p=trace->pairs[i];
        if (i==2) TraceDisplay(p);
        else TraceDisplay(p,i==3 ? 0 : (i==4 ? 153456789:116456789));
        const auto first_qpc=p.display_accept_qpc;
        TraceDisplay(p,999999999);
        assert(p.display_reads==2 && p.display_accept_qpc==first_qpc);
    }
    trace->requested_queue_priority=D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    trace->effective_queue_priority=D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    trace->queue_priority_reason="high-unsupported";
    trace->Write(2040,2232,1530,1674,true,false);
    trace->Write(2040,2232,1530,1674,true,false); // append, never truncate
    trace->path=path.parent_path()/L"absent-parent"/L"no.jsonl";
    trace->Write(1,1,1,1,false,true); // unavailable output must not throw
}
'''


def main():
    prefix, marker, _ = SOURCE.read_text(encoding="utf-8").partition("void Check(HRESULT result,const char* stage)")
    assert marker, "private trace helper boundary changed"
    with tempfile.TemporaryDirectory(prefix="kotor-neural-trace-cpu-") as temp:
        folder = Path(temp)
        source = folder / "trace_cpu.cpp"
        source.write_text(prefix + MAIN, encoding="utf-8")
        executable = folder / "trace_cpu.exe"
        flags = ["cl.exe", "/nologo", "/std:c++20", "/EHsc", "/W4", "/WX",
                 "/DNOMINMAX", "/DWIN32_LEAN_AND_MEAN", "/I", str(ROOT / "src/host64/include"),
                 "/I", str(ROOT / "src/common"), str(source), "/Fe:" + str(executable)]
        command = f'"{VCVARS}" >nul && ' + subprocess.list2cmdline(flags)
        # cmd /c uses quote stripping, not the C-runtime argv escaping applied
        # by subprocess to a list (which would turn the quotes into backslashes).
        subprocess.run(f'cmd.exe /d /s /c "{command}"', cwd=folder, check=True)
        output = folder / "trace.jsonl"
        subprocess.run([str(executable), str(output)], cwd=folder, check=True)
        rows = [json.loads(line) for line in output.read_text().splitlines()]
        assert len(rows) == 722 and rows[:361] == rows[361:]
        session, *pairs = rows[:361]
        assert session["records"] == 360 and session["admissions"] == 365
        assert session["gpu_timestamps"] is False and session["qpc_frequency"] > 0
        assert session["work_width"] == 1530 and session["prefetch"] is True
        assert session["requested_queue_priority"] == "HIGH"
        assert session["effective_queue_priority"] == "NORMAL"
        assert session["queue_priority_reason"] == "high-unsupported"
        assert [p["ready_value"] for p in pairs] == list(range(100, 460))
        assert all(p["xr_frame_id"] == 91 and p["generation"] == 3 for p in pairs)
        assert all(p["trace_qpc"] == session["trace_qpc"] for p in pairs)
        for i, pair in enumerate(pairs):
            source_ns = pair["predicted_display_time_ns"]
            admission_ns = pair["admission_predicted_display_time_ns"]
            assert source_ns == 123456789
            assert type(admission_ns) is int
            if i % 4 < 2:
                assert admission_ns == 0  # unavailable, not a measured age
            else:
                expected_age_ns = 30000000 if i % 4 == 2 else -7000000
                assert admission_ns == source_ns + expected_age_ns
                assert admission_ns - source_ns == expected_age_ns
        assert pairs[0]["left"]["worker_pid"] == 111 and pairs[0]["right"]["worker_pid"] == 222
        assert pairs[0]["left"]["worker_sequence"] == pairs[0]["right"]["worker_sequence"] == 17
        assert pairs[0]["replaced_input"] is True and pairs[1]["superseded_output"] is True
        assert all(p["compose_end_qpc"] == 0 for p in pairs)
        for i, p in enumerate(pairs):
            assert p["display_predicted_display_time_ns"] == {4: 153456789, 5: 116456789}.get(i, 0)
            assert p["display_reads"] == (2 if 2 <= i < 6 else 0)
            assert (p["display_accept_qpc"] > 0) == (2 <= i < 6)
    print("PASS: env modes, bounded/stable 360 records, exact IDs/PID/IPC sequence, JSON/append, XR admission timestamps/legacy zero/negative age, missing stamps, failed export")


if __name__ == "__main__":
    main()
