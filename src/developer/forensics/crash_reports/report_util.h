// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FORENSICS_CRASH_REPORTS_REPORT_UTIL_H_
#define SRC_DEVELOPER_FORENSICS_CRASH_REPORTS_REPORT_UTIL_H_

#include <fuchsia/feedback/cpp/fidl.h>
#include <fuchsia/mem/cpp/fidl.h>
#include <lib/fit/result.h>

#include <map>
#include <optional>
#include <string>

#include "src/developer/forensics/crash_reports/product.h"
#include "src/developer/forensics/utils/errors.h"

namespace forensics {
namespace crash_reports {

// Shorten |program_name| into a shortname by removing the "fuchsia-pkg://" prefix if present and
// replacing all '/' with ':'.
//
// For example `fuchsia-pkg://fuchsia.com/crash-reports#meta/crash_reports.cmx` becomes
// `fuchsia.com:crash-reports#meta:crash_reports.cmx`
std::string Shorten(std::string program_name);

// Builds the final set of annotations and attachments to attach to the Crashpad report.
//
// * Most annotations are shared across all crash reports, e.g., |bugreport|.annotations().
// * Some annotations are report-specific, e.g., Dart exception type.
// * Adds any annotations from |report|.
//
// * Most attachments are shared across all crash reports, e.g., |bugreport|.bugreport().
// * Some attachments are report-specific, e.g., Dart exception stack trace.
// * Adds any attachments from |report|.
void BuildAnnotationsAndAttachments(fuchsia::feedback::CrashReport report,
                                    ::fit::result<fuchsia::feedback::Bugreport, Error> bugreport,
                                    const std::optional<zx::time_utc>& current_time,
                                    const ::fit::result<std::string, Error>& device_id,
                                    const ErrorOr<std::string>& os_version, const Product& product,
                                    std::map<std::string, std::string>* annotations,
                                    std::map<std::string, fuchsia::mem::Buffer>* attachments,
                                    std::optional<fuchsia::mem::Buffer>* minidump);

}  // namespace crash_reports
}  // namespace forensics

#endif  // SRC_DEVELOPER_FORENSICS_CRASH_REPORTS_REPORT_UTIL_H_
