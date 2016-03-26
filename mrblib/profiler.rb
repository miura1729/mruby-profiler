module Profiler
  def self.analyze
    files = {}
    nosrc = {}
    ftab = {}
    irep_num.times do |ino|
      fn = get_inst_info(ino, 0)[0]
      if fn.is_a?(String) then
        files[fn] ||= {}
        ilen(ino).times do |ioff|
          info = get_inst_info(ino, ioff)
          if info[1] then
            files[fn][info[1]] ||= []
            files[fn][info[1]].push info
          end
        end
      else
        mname = "#{fn[0]}##{fn[1]}"
        ilen(ino).times do |ioff|
          info = get_inst_info(ino, ioff)
          nosrc[mname] ||= []
          nosrc[mname].push info
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

        #   Execute Count
#        print(sprintf("%04d %10d %s", i, num, lin))

        #   Execute Time
        print(sprintf("%04d %7.5f %s", i, time, lin))

        #   Execute Time per 1 instruction
#        if num != 0 then
#          print(sprintf("%04d %4.5f %s", i, time / num, lin))
#        else
#          print(sprintf("%04d %4.5f %s", i, 0.0, lin))
#        end
        if infos[i + 1] then
          codes = {}
          infos[i + 1].each do |info|
            codes[info[4]] ||= [nil, 0, 0.0]
            codes[info[4]][0] = info[5]
            codes[info[4]][1] += info[2]
            codes[info[4]][2] += info[3]
          end

          codes.each do |val|
            code = val[1][0]
            num = val[1][1]
            time = val[1][2]
            printf("            %10d %-7.5f    %s \n" , num, time, code)
          end
        end
      end
    end

    nosrc.each do |mn, infos|
      codes = {}
      infos.each do |info|
        codes[info[4]] ||= [nil, 0, 0.0]
        codes[info[4]][0] = info[5]
        codes[info[4]][1] += info[2]
        codes[info[4]][2] += info[3]
      end
 
      print("#{mn} \n")
      codes.each do |val|
        code = val[1][0]
        num = val[1][1]
        time = val[1][2]
        printf("            %10d %-7.5f    %s \n" , num, time, code)
      end
    end
  end
end
