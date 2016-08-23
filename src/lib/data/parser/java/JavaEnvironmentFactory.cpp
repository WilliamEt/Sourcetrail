#include "data/parser/java/JavaEnvironmentFactory.h"

#include <cstdlib>

#ifdef __APPLE__
#include <dlfcn.h>
#endif

#include <jni.h>

#include "data/parser/java/JavaEnvironment.h"
#include "settings/ApplicationSettings.h"
#include "utility/logging/logging.h"
#include "utility/messaging/type/MessageStatus.h"
#include "utility/file/FileSystem.h"

void JavaEnvironmentFactory::createInstance(std::string classPath)
{
	if (s_instance)
	{
		if (classPath == s_classPath)
		{
			return;
		}
		else
		{
			LOG_ERROR("java classpath cannot be changed!");
			// todo: implement destroying the old factory instance and create a new one.
			// may be not so easy... there can only be one java env per process (which can never be destroyed) -.-
			return;
		}
	}

	bool javaFound = false;

#ifdef _WIN32
	{
		std::string oldPathContent = getenv("path");

		std::string javapath = ApplicationSettings::getInstance()->getJavaPath() + "/client/";
		putenv(("path=" + oldPathContent + ";" + javapath).c_str()); // path env is only modified in the scope of this process.

		javaFound = FileSystem::exists(javapath + "jvm.dll");
	}
#endif

#ifdef __APPLE__
	{
		std::string javapath = ApplicationSettings::getInstance()->getJavaPath();
		void* handle = nullptr;

		if (javapath.size())
		{
			handle = dlopen((javapath + "/jre/lib/server/libjvm.dylib").c_str(), RTLD_NOW);
		}

		if (!handle && javapath.size())
		{
			handle = dlopen((javapath + "/libjvm.dylib").c_str(), RTLD_NOW);
		}

		if (!handle)
		{
			handle = dlopen("libjvm.dylib", RTLD_NOW);
		}

		if (handle)
		{
			javaFound = true;
		}
	}
#endif

	if (!javaFound)
	{
		std::string errorMessage = "Unable to locate Java on this machine.";
		LOG_ERROR(errorMessage);
		MessageStatus(errorMessage, true, false).dispatch();
		return;
	}

	s_classPath = classPath;

	JavaVM* jvm = nullptr;				// Pointer to the JVM (Java Virtual Machine)
	JNIEnv* env = nullptr;				// Pointer to native interface

	JavaVMInitArgs vm_args;                        // Initialization arguments
	JavaVMOption* options = new JavaVMOption[3];   // JVM invocation options
	std::string classPathOption = "-Djava.class.path=" + classPath;
	options[0].optionString = const_cast<char*>(classPathOption.c_str());
	options[1].optionString = const_cast<char*>("-Xms1m");
	std::string maximumMemoryOprionString = "-Xmx" + std::to_string(ApplicationSettings::getInstance()->getJavaMaximumMemory()) + "m";
	options[2].optionString =  const_cast<char*>(maximumMemoryOprionString.c_str());
	vm_args.version = JNI_VERSION_1_6;
	vm_args.nOptions = 3;
	vm_args.options = options;
	vm_args.ignoreUnrecognized = false;     // invalid options make the JVM init fail

	jint rc = JNI_CreateJavaVM(&jvm, (void**)&env, &vm_args);

	delete [] options;

	if(rc != JNI_OK)
	{
		std::string errorMessage;
		if(rc == JNI_EVERSION)
		{
			errorMessage = "JVM is oudated and doesn't meet requirements";
		}
		else if(rc == JNI_ENOMEM)
		{
			errorMessage = "not enough memory for JVM";
		}
		else if(rc == JNI_EINVAL)
		{
			errorMessage = "invalid argument for launching JVM";
		}
		else if(rc == JNI_EEXIST)
		{
			errorMessage = "the process can only launch one JVM an not more";
		}
		else
		{
			errorMessage = "could not create the JVM instance (error code " + std::to_string(rc) + ")";
		}

		LOG_ERROR(errorMessage);
		MessageStatus("Error while creating Java environment: " + errorMessage, true, false).dispatch();
	}
	else
	{
		jvm->DetachCurrentThread();
		s_instance = std::shared_ptr<JavaEnvironmentFactory>(new JavaEnvironmentFactory(jvm));
	}
}

std::shared_ptr<JavaEnvironmentFactory> JavaEnvironmentFactory::getInstance()
{
	return s_instance;
}

JavaEnvironmentFactory::~JavaEnvironmentFactory()
{
// todo: what if there are threads running using the jvm?? log something!
	m_jvm->DestroyJavaVM();
}

std::shared_ptr<JavaEnvironment> JavaEnvironmentFactory::createEnvironment()
{
	std::thread::id currentThreadId = std::this_thread::get_id();

	JNIEnv* env;

	{
		std::lock_guard<std::mutex> lock(m_threadIdToEnvAndUserCountMutex);

		std::map<std::thread::id, std::pair<JNIEnv*, int>>::const_iterator it = m_threadIdToEnvAndUserCount.find(currentThreadId);
		if (it != m_threadIdToEnvAndUserCount.end())
		{
			env = it->second.first;
		}
		else
		{
			m_jvm->AttachCurrentThread((void**)&env, NULL);
			m_threadIdToEnvAndUserCount.insert(std::make_pair(currentThreadId, std::make_pair(env, 0)));
		}
	}

	return std::shared_ptr<JavaEnvironment>(new JavaEnvironment(m_jvm, env));
}

std::shared_ptr<JavaEnvironmentFactory> JavaEnvironmentFactory::s_instance;

std::string JavaEnvironmentFactory::s_classPath;

JavaEnvironmentFactory::JavaEnvironmentFactory(JavaVM* jvm)
	: m_jvm(jvm)
{
}

void JavaEnvironmentFactory::registerEnvironment()
{
	std::thread::id currentThreadId = std::this_thread::get_id();
	{
		std::lock_guard<std::mutex> lock(m_threadIdToEnvAndUserCountMutex);
		std::map<std::thread::id, std::pair<JNIEnv*, int>>::iterator it = m_threadIdToEnvAndUserCount.find(currentThreadId);
		if (it != m_threadIdToEnvAndUserCount.end())
		{
			it->second.second++;
		}
		else
		{
			LOG_ERROR("something went horribly wrong while registering a java environment");
		}
	}
}

void JavaEnvironmentFactory::unregisterEnvironment()
{
	std::thread::id currentThreadId = std::this_thread::get_id();
	{
		std::lock_guard<std::mutex> lock(m_threadIdToEnvAndUserCountMutex);
		std::map<std::thread::id, std::pair<JNIEnv*, int>>::iterator it = m_threadIdToEnvAndUserCount.find(currentThreadId);
		if (it != m_threadIdToEnvAndUserCount.end())
		{
			it->second.second--;
			if (it->second.second == 0)
			{ // TODO: currently this happens quite often. do something about that.
				m_jvm->DetachCurrentThread();
				m_threadIdToEnvAndUserCount.erase(it);
			}
		}
		else
		{
			LOG_ERROR("something went horribly wrong while unregistering a java environment");
		}
	}
}

