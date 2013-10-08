module Profiler
  def self.analyze
    files = {}
    ftab = {}
    irep_len.times do |ino|
      fn = get_prof_info(ino, 0)[0]
      if fn then
        files[fn] ||= {}
        ilen(ino).times do |ioff|
          info = get_prof_info(ino, ioff)
          if info[1] then
            files[fn][info[1]] ||= []
            files[fn][info[1]].push info
          end
        end
      end
    end

    files.each do |fn, infos|
      lines = read(fn)
      lines.each_with_index do |lin, i|
        num = 0
        time = 0.0
        if infos[i + 1] then
          num = infos[i + 1][0][2]
          infos[i + 1].each do |info|
            time += info[3]
          end
        end
#        print(sprintf("%04d %10d %s", i, num, lin))
        print(sprintf("%04d %4.5f %s", i, time, lin))
      end
    end
  end
end
