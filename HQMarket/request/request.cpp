#include "request.pb.h"
#include "request.h"
#include "../common/mapped_value.h"

static request::RequestType ToProtoType(CRequest::Type type)
{
	switch (type)
	{
	case CRequest::Type::QUERY_AUTH:
		return request::RequestType::QUERY_AUTH;
	case CRequest::Type::QUERY_USERINFO:
		return request::RequestType::QUERY_USERINFO;
	case CRequest::Type::UPDATE_AUTH:
		return request::RequestType::UPDATE_AUTH;
	case CRequest::Type::UPDAT_PRODUCT:
		return request::RequestType::UPDAT_PRODUCT;
	case CRequest::Type::UNKNOWN:
	default:
		return request::RequestType::UNKNOWN;
	}
}

CRequest::CRequest() : m_arena(std::make_unique<google::protobuf::Arena>())
{
	m_data = google::protobuf::Arena::CreateMessage<request::RequestData>(m_arena.get());
}

CRequest::~CRequest() = default;

bool CRequest::Serialize(std::string* output) const
{
	if (output == nullptr)
	{
		return false;
	}
	if (!m_strPayload.empty())
	{
		*output = m_strPayload;
		return true;
	}
	if ((m_data == nullptr) || (GetCmd() == "connet_build"))
	{
		return false;
	}
	return m_data->SerializeToString(output);
}

bool CRequest::Deserialize(const std::string& data)
{
	m_strPayload = data;
	return true;
}

void CRequest::SetConnectionId(evutil_socket_t connectionId)
{
	m_connectionId = connectionId;
}

evutil_socket_t CRequest::GetConnectionId() const
{
	return m_connectionId;
}

const std::string& CRequest::GetPayload() const
{
	return m_strPayload;
}

void CRequest::SetType(CRequest::Type type)
{
	if (m_data == nullptr)
	{
		return;
	}
	m_data->set_type(static_cast<request::RequestType>(ToProtoType(type)));
}

void CRequest::SetCmd(const std::string& strCmd)
{
	if (m_data == nullptr)
	{
		return;
	}
	m_data->set_cmd(strCmd);
}

void CRequest::SetExtraData(const std::string& strKey, const std::string& strVal)
{
	if (m_data == nullptr)
	{
		return;
	}
	(*m_data->mutable_extra())[strKey] = strVal;
}

void CRequest::SetReturnData(const std::string& strKey, const std::string& strVal)
{
	if (m_data == nullptr)
	{
		return;
	}
	(*(m_data->mutable_ret()))[strKey] = strVal;
}

CRequest::Type CRequest::GetType() const
{
	if (m_data == nullptr)
	{
		return CRequest::Type::UNKNOWN;
	}
	return static_cast<CRequest::Type>(m_data->type());
}

const std::string& CRequest::GetCmd() const
{
	if (m_data == nullptr)
	{
		static const std::string emptyValue;
		return emptyValue;
	}
	return m_data->cmd();
}

std::unordered_map<std::string, std::string> CRequest::GetExtraData() const
{
	if (m_data == nullptr)
	{
		return {};
	}
	return {m_data->extra().begin(), m_data->extra().end()};
}

const std::string& CRequest::GetExtraData(const std::string& strKey) const
{
	if (m_data == nullptr)
	{
		static const std::string emptyValue;
		return emptyValue;
	}
	return container::FindMappedValue(m_data->extra(), strKey);
}

const std::string& CRequest::GetReturnData(const std::string& strKey) const
{
	if (m_data == nullptr)
	{
		static const std::string emptyValue;
		return emptyValue;
	}
	return container::FindMappedValue(m_data->ret(), strKey);
}

std::unordered_map<std::string, std::string> CRequest::GetReturnData() const
{
	if (m_data == nullptr)
	{
		return {};
	}
	return {m_data->ret().begin(), m_data->ret().end()};
}
