module Profiler
  #Perform analysis on the collected profile information
  #
  # Note: if profiling an embedded mruby instance be aware that the execution
  #       time of the return instruction leaving the mruby VM will be
  #       overestimated
  def self.analyze
    if ARGV[0] == "-k" then
      analyze_kcached
    else
      analyze_normal
    end
  end

  #Produce a kcachegrind compatiable output to STDOUT
  #
  #Note: There appears to be some issue in the output including:
  #      1. Multiple traces of the same method (with different callstacks)
  #      2. Incorrect estimates of cumulative call costs (see lines after
  #         'calls='
  #      3. Multiple methods using the same IREP sequence (it's unclear if mruby
  #         is mapping different methods to the same IREP instance if the locals
  #         and VM code sequence is the same. If it is, then IREP pointers are
  #         no longer a valid UUID for a method call).
  def self.analyze_kcached
    ireps = {}
    print("version: 1\n")
    print("positions: address\n")
    print("events: ticks\n")

    #Build map of irep addresses to alias numbers
    irep_num.times do |ino|
      insir = get_irep_info(ino)
      ireps[insir[0]] = ino
      print("fl=(#{ino}) #{insir[3]}\n") if insir[3]
      print("fn=(#{ino}) #{insir[1]}##{insir[2]}\n")
    end

    irep_num.times do |ino|
      insir = get_irep_info(ino)
      irepno = ireps[insir[0]]
      print("fl=(#{irepno})#{insir[3]}\n") if insir[3]
      print("fn=(#{irepno})#{insir[1]}##{insir[2]}\n")

      ilen(ino).times do |ioff|
        insin = get_inst_info(ino, ioff)
        print("0x#{insir[0]} #{insin[1]} #{(insin[3] * 10000000).to_i}\n")
      end

      childs = insir[4]
      ccalls = insir[5]
      childs.size.times do |cno|
        irepno = ireps[childs[cno]]
        irep = get_irep_info(irepno)
        print("cfl=(#{irepno}) #{irep[3]}\n") if irep[3]
        print("cfn=(#{irepno}) #{irep[1]}##{irep[2]}\n")
        print("calls=#{ccalls[cno]} +1\n")
        print("#{irep[0]} 1000\n")
      end
    end
  end

  #Display normal mixed source level/VM level analysis of traced results
  #
  #The default format is:
  #
  #LINE TIME_SECONDS SOURCE_LINE
  #     NUM_EXECUTIONS TIME_SECONDS DECODED_VM_INSTRUCTION
  def self.analyze_normal

    #Known source
    files = {}
    #Methods without corresponding source
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

    #Print stats for each line and disassembled VM instructions
    #which correspond to each line
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

    #Dump stats for lines without any source level information
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
