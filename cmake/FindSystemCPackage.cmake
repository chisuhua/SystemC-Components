# a small wrapper to map SLS, OSCI SystemC (from environment) and conan package to the same variables
if(HAS_PCT OR USE_PCT)
	find_package(SLSSystemC REQUIRED)
else()
	find_package(SystemCLanguage QUIET)
	if(TARGET SystemC::systemc)
		set(SystemC_FOUND true)
		set(SystemC_INCLUDE_DIRS ${SystemC_systemc_INCLUDE_DIRS})
	    set(SystemC_LIBRARIES SystemC::SystemC)
		find_package(systemc-scv QUIET)
        if(systemc-scv_FOUND)
          set(CCI_FOUND TRUE)
          set(CCI_INCLUDE_DIRS ${systemc-scv_INCLUDE_DIRS})
          set(CCI_LIBRARIES systemc-scv::systemc-scv)
        endif()
		find_package(systemc-cci QUIET)
        if(systemc-cci_FOUND)
          set(CCI_FOUND TRUE)
          set(CCI_INCLUDE_DIRS ${systemc-cci_INCLUDE_DIRS})
          set(CCI_LIBRARIES systemc-cci::systemc-cci)
        endif()
	elseif(TARGET CONAN_PKG::systemc)
		set(SystemC_FOUND true)
		set(SystemC_INCLUDE_DIRS ${CONAN_INCLUDE_DIRS_SYSTEMC})
	    set(SystemC_LIBRARIES CONAN_PKG::systemc)
	    if(TARGET CONAN_PKG::systemc-scv)
          set(SCV_FOUND TRUE)
          set(SCV_INCLUDE_DIRS ${CONAN_INCLUDE_DIRS_SYSTEMC-SCV})
          set(SCV_LIBRARIES CONAN_PKG::systemc-scv)
	    endif()
        if(TARGET CONAN_PKG::systemc-cci)
          set(CCI_FOUND TRUE)
          set(CCI_INCLUDE_DIRS ${CONAN_INCLUDE_DIRS_SYSTEMC-CCI})
          set(CCI_LIBRARIES CONAN_PKG::systemc-cci)
        endif()
    else()
    	find_package(OSCISystemC)
	endif()
endif()

## missing old SystemC packages
#if(TARGET SystemCVerification::SystemCVerification)
#    set(SCV_FOUND TRUE)
#    set(SCV_LIBRARIES SystemCVerification::SystemCVerification)
#endif()
#if(TARGET SystemC-CCI::SystemC-CCI)
#    set(CCI_FOUND TRUE)
#    set(CCI_LIBRARIES SystemC-CCI::SystemC-CCI)
#endif()
