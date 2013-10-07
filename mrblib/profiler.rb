module Profiler
  def self.analyze
    p irep_len
    p ilen(0)
    p get_prof_info(100, 0)
  end
end
