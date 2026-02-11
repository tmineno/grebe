#include "stages/data_source_adapter.h"

namespace grebe {

DataSourceAdapter::DataSourceAdapter(IDataSource& source)
    : source_(source) {}

StageResult DataSourceAdapter::process(const BatchView& /*in*/, BatchWriter& out,
                                       ExecContext& /*ctx*/) {
    auto result = source_.read_frame(fb_);

    switch (result) {
    case ReadResult::Ok: {
        Frame frame = Frame::from_frame_buffer(fb_);
        // Propagate sample_rate from source info
        frame.sample_rate_hz = source_.info().sample_rate_hz;
        out.push(std::move(frame));
        return StageResult::Ok;
    }
    case ReadResult::NoData:
        return StageResult::NoData;
    case ReadResult::EndOfStream:
        return StageResult::EOS;
    case ReadResult::Error:
        return StageResult::Error;
    }

    return StageResult::Error;
}

} // namespace grebe
