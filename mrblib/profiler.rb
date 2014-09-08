module Profiler
OPCODE = [
  :NOP, :MOVE, :LOADL, :LOADI, :LOADSYM, :LOADNIL, :LOADSELF, :LOADT, :LOADF,
  :GETGLOBAL, :SETGLOBAL, :GETSPECIAL, :SETSPECIAL,
  :GETIV, :SETIV, :GETCV, :SETCV,
  :GETCONST, :SETCONST, :GETMCNST, :SETMCNST, :GETUPVAR, :SETUPVAR,

  :JMP, :JMPIF, :JMPNOT,
  :ONERR, :RESCUE, :POPERR, :RAISE, :EPUSH, :EPOP,

  :SEND, :SENDB, :FSEND, :CALL, :SUPER, :ARGARY, :ENTER, :KARG, :KDICT,

  :RETURN, :TAILCALL, :BLKPUSH,

  :ADD, :ADDI, :SUB, :SUBI, :MUL, :DIV,
  :EQ, :LT, :LE, :GT, :GE,

  :ARRAY, :ARYCAT, :ARYPUSH, :AREF, :ASET, :APOST,

  :STRING, :STRCAT,

  :HASH,
  :LAMBDA,
  :RANGE,

  :OCLASS, :CLASS, :MODULE, :EXEC, :METHOD, :SCLASS,
  :TCLASS,

  :DEBUG, :STOP, :ERR,

  :RSVD1, :RSVD2, :RSVD3, :RSVD4, :RSVD5,
]

  def self.analyze
    files = {}
    nosrc = {}
    ftab = {}
    irep_len.times do |ino|
      fn = get_prof_info(ino, 0)[0]
      if fn.is_a?(String) then
        files[fn] ||= {}
        ilen(ino).times do |ioff|
          info = get_prof_info(ino, ioff)
          if info[1] then
            files[fn][info[1]] ||= []
            files[fn][info[1]].push info
          end
        end
      else
        mname = "#{fn[0]}##{fn[1]}"
        ilen(ino).times do |ioff|
          info = get_prof_info(ino, ioff)
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
            printf("            %10d %-7.5f    #{OPCODE[code & 0x7f]} \n" , num, time)
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
        printf("            %10d %-7.5f    #{OPCODE[code & 0x7f]} \n" , num, time)
      end
    end
  end
end
