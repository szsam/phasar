/**
 * @author Sebastian Roland <seroland86@gmail.com>
 */

#ifndef PHASAR_PHASARLLVM_DATAFLOWSOLVER_IFDSIDE_IFDSFIELDSENSTAINTANALYSIS_STATS_TRACESTATSWRITER_H
#define PHASAR_PHASARLLVM_DATAFLOWSOLVER_IFDSIDE_IFDSFIELDSENSTAINTANALYSIS_STATS_TRACESTATSWRITER_H

#include "TraceStats.h"

#include "../Utils/Log.h"

#include <fstream>
#include <string>

namespace psr {

class TraceStatsWriter {
public:
  TraceStatsWriter(const TraceStats &_traceStats, const std::string _outFile)
      : traceStats(_traceStats), outFile(_outFile) {}
  virtual ~TraceStatsWriter() = default;

  virtual void write() const = 0;

protected:
  const TraceStats &getTraceStats() const { return traceStats; }
  const std::string getOutFile() const { return outFile; }

private:
  const TraceStats &traceStats;
  const std::string outFile;
};

} // namespace psr

#endif
