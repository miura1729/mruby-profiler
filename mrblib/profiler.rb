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
          infos[i + 1].each do |info|
            time += info[3]
            if num < info[2] then
              num = info[2]
            end
          end
        end
#        print(sprintf("%04d %10d %s", i, num, lin))
        print(sprintf("%04d %4.5f %s", i, time, lin))
#        if num != 0 then
#          print(sprintf("%04d %4.5f %s", i, time / num, lin))
#        else
#          print(sprintf("%04d %4.5f %s", i, 0.0, lin))
#        end
      end
    end
  end
end
